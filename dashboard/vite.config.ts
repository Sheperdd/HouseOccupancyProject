import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Dev-mode proxy: the browser talks only to Vite (5173); REST and the
// WebSocket are forwarded to the backend on 8000. In production the
// backend serves dashboard/dist itself, so paths stay relative either way.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // 127.0.0.1, not localhost: Node resolves localhost to IPv6 ::1 first,
      // but uvicorn listens on IPv4 only -> ECONNREFUSED ::1:8000.
      "/api": "http://127.0.0.1:8000",
      "/ws": { target: "ws://127.0.0.1:8000", ws: true },
    },
  },
});
