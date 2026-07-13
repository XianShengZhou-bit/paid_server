function trimTrailingSlash(value) {
  return value.endsWith("/") ? value.slice(0, -1) : value;
}

// API / WebSocket 经 Nginx 同源反代到网关，不再直连 :9993 / :9992
const sameOrigin = window.location.origin;
const wsOrigin =
  (window.location.protocol === "https:" ? "wss://" : "ws://") + window.location.host;

export const GATEWAY_HTTP_ORIGIN = trimTrailingSlash(
  window.__GATEWAY_HTTP_ORIGIN__ || sameOrigin
);

export const GATEWAY_WS_ORIGIN = trimTrailingSlash(
  window.__GATEWAY_WS_ORIGIN__ || wsOrigin
);
