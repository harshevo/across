import { createServer } from "node:http";

const port = Number.parseInt(process.env.PORT ?? "4198", 10);
const server = createServer((request, response) => {
  response.setHeader("content-type", "application/json");
  if (request.url === "/health") {
    response.end(JSON.stringify({ ok: true, release: "one" }));
    return;
  }
  response.end(JSON.stringify({ message: "Across Node release one" }));
});

server.listen(port, "127.0.0.1");

const shutdown = () => server.close(() => process.exit(0));
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
