# Self-hosted netplay lobby — the same Node adapter the test suites run
# (src/infrastructure/local/node-lobby.mjs). Room semantics live in
# src/domain/ and are identical to the deployed Cloudflare worker.
#
#   docker build -t ssb64-lobby .
#   docker run -p 8799:8799 -v lobby-data:/data ssb64-lobby
#
# State (leaderboard.json, brackets.json, replays/, crashes.jsonl) lands
# in LOBBY_DATA_DIR — mount a volume at /data to keep it across restarts.
# All store writes are atomic temp+rename, so an ungraceful stop can't
# leave torn JSON behind.
FROM node:22-alpine

ENV NODE_ENV=production \
    PORT=8799 \
    LOBBY_DATA_DIR=/data

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev
COPY src ./src

EXPOSE 8799
VOLUME /data
USER node

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD wget -qO- http://127.0.0.1:${PORT}/health || exit 1

CMD ["node", "src/infrastructure/local/node-lobby.mjs"]
