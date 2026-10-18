/*
 * lobby_json_test.cpp — wire-JSON suite for the lobby message helpers
 * (src/networking/netplay/session/lobby_json.*). Covers the cases the old
 * substring scanner got wrong: escaped quotes inside strings, a key name
 * appearing inside a string value, nested objects/arrays, non-string
 * scalars, and malformed input. Self-contained: links only lobby_json.cpp,
 * no game state, no network.
 */

#include "networking/netplay/session/lobby_json.h"
#include "smash_test/check.h"

#include <string>

using ssb64::netlobby::json_int;
using ssb64::netlobby::json_parse;
using ssb64::netlobby::json_str;

int main()
{
	/* ---- well-formed flat object (the common wire shape) ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"type\":\"welcome\",\"room\":\"ABCD12\",\"slot\":2,"
			"\"ready\":true,\"public\":\"1.2.3.4:5555\"}");
		SMASH_CHECK(json_str(doc, "type") == "welcome");
		SMASH_CHECK(json_str(doc, "room") == "ABCD12");
		SMASH_CHECK(json_int(doc, "slot") == 2);
		/* bool reads back as its wire token so == "true" checks work */
		SMASH_CHECK(json_str(doc, "ready") == "true");
		SMASH_CHECK(json_int(doc, "ready") == -1);   /* not numeric */
		SMASH_CHECK(json_str(doc, "public") == "1.2.3.4:5555");
		SMASH_CHECK(json_str(doc, "missing").empty());
		SMASH_CHECK(json_int(doc, "missing") == -1);
		SMASH_CHECK(json_int(doc, "missing", 7) == 7);
	}

	/* ---- escaped quotes: the scanner cut the value at the first '"' ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"message\":\"he said \\\"slot\\\":9 inside\",\"slot\":1}");
		SMASH_CHECK(json_str(doc, "message") == "he said \"slot\":9 inside");
		/* the real field wins — the embedded text must not be seen as a key */
		SMASH_CHECK(json_int(doc, "slot") == 1);
	}

	/* ---- key name inside a string value must not leak into lookups ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"note\":\"prefix \\\"public\\\":\\\"evil\\\" suffix\","
			"\"public\":\"real\"}");
		SMASH_CHECK(json_str(doc, "public") == "real");
	}

	/* ---- nested objects/arrays: non-scalars read as empty ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"players\":[{\"slot\":0}],\"meta\":{\"x\":1},\"tag\":null}");
		SMASH_CHECK(json_str(doc, "players").empty());
		SMASH_CHECK(json_str(doc, "meta").empty());
		SMASH_CHECK(json_str(doc, "tag").empty());
		SMASH_CHECK(json_int(doc, "players") == -1);
	}

	/* ---- string-encoded numbers keep the old atoi() contract ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"slot\":\"3\",\"float\":2.9,\"neg\":-4}");
		SMASH_CHECK(json_int(doc, "slot") == 3);
		SMASH_CHECK(json_str(doc, "slot") == "3");
		SMASH_CHECK(json_int(doc, "float") == 2);    /* truncation, not round */
		SMASH_CHECK(json_int(doc, "neg") == -4);
	}

	/* ---- malformed input: discarded, every read empty, no throw ---- */
	{
		const nlohmann::json doc = json_parse("{\"type\":\"welcome\",garbage");
		SMASH_CHECK(doc.is_discarded());
		SMASH_CHECK(json_str(doc, "type").empty());
		SMASH_CHECK(json_int(doc, "slot") == -1);

		const nlohmann::json empty = json_parse("");
		SMASH_CHECK(empty.is_discarded());
		SMASH_CHECK(json_str(empty, "type").empty());

		const nlohmann::json notobj = json_parse("[1,2,3]");
		SMASH_CHECK(!notobj.is_discarded());
		SMASH_CHECK(json_str(notobj, "type").empty());
	}

	/* ---- roster-shaped payload: fields live inside the players array ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"type\":\"peers\",\"players\":["
			"{\"slot\":0,\"role\":\"host\",\"public\":\"10.0.0.1:1\",\"ready\":true},"
			"{\"slot\":4,\"role\":\"spec\",\"public\":\"-\",\"ready\":false}]}");
		const auto it = doc.find("players");
		SMASH_EXPECT(it != doc.end() && it->is_array());
		SMASH_CHECK(it->size() == 2);
		const nlohmann::json& host = (*it)[0];
		const nlohmann::json& spec = (*it)[1];
		SMASH_CHECK(json_int(host, "slot") == 0);
		SMASH_CHECK(json_str(host, "role") == "host");
		SMASH_CHECK(json_str(host, "ready") == "true");
		SMASH_CHECK(json_int(spec, "slot") == 4);
		SMASH_CHECK(json_str(spec, "role") == "spec");
		SMASH_CHECK(json_str(spec, "ready") == "false");
	}

	/* ---- a "slot": inside a nested string must not appear as an entry ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"type\":\"peers\",\"players\":["
			"{\"slot\":1,\"role\":\"join\",\"public\":\"10.0.0.2:9\"}],"
			"\"debug\":\"contains \\\"slot\\\":99 text\"}");
		const auto it = doc.find("players");
		SMASH_EXPECT(it != doc.end() && it->is_array() && it->size() == 1);
		SMASH_CHECK(json_int((*it)[0], "slot") == 1);
		SMASH_CHECK(json_str(doc, "debug") == "contains \"slot\":99 text");
	}

	/* ---- non-object entries inside the players array are skippable ---- */
	{
		const nlohmann::json doc = json_parse(
			"{\"type\":\"peers\",\"players\":[7,\"junk\",{\"slot\":2}]}");
		const auto it = doc.find("players");
		SMASH_EXPECT(it != doc.end() && it->is_array() && it->size() == 3);
		SMASH_CHECK(!(*it)[0].is_object());
		SMASH_CHECK(!(*it)[1].is_object());
		SMASH_CHECK((*it)[2].is_object());
		SMASH_CHECK(json_int((*it)[2], "slot") == 2);
	}

	return smash_test_report("lobby_json_test");
}
