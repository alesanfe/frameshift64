#pragma once

/* Community panel renderers used by PortMenuOnline.cpp's netplay lobby.
 * Defined in PortMenuOnlineBoards.cpp — file-local contract, not a
 * public menu API. */

namespace ssb64 {

void SaveOnlineCvars();
void RenderOnlineFriends();
void RenderOnlineLeaderboard(const char *serverBuf);
void RenderOnlineTournaments(const char *serverBuf);

} // namespace ssb64
