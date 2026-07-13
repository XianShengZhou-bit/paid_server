import { GATEWAY_HTTP_ORIGIN } from "./config.js";

function buildUrl(path) {
  return `${GATEWAY_HTTP_ORIGIN}${path}`;
}

export class ApiError extends Error {
  constructor(code, message, httpStatus) {
    super(message || "请求失败");
    this.name = "ApiError";
    this.code = code;
    this.httpStatus = httpStatus;
  }
}

async function requestJson(method, path, body, withCredentials) {
  const response = await fetch(buildUrl(path), {
    method,
    headers: { "Content-Type": "application/json" },
    credentials: withCredentials ? "include" : "omit",
    body: body ? JSON.stringify(body) : undefined,
  });

  let envelope;
  try {
    envelope = await response.json();
  } catch (error) {
    throw new ApiError(-1, "响应不是合法 JSON", response.status);
  }

  if (!envelope || typeof envelope.code !== "number") {
    throw new ApiError(-1, "响应格式错误", response.status);
  }

  if (envelope.code !== 0) {
    throw new ApiError(envelope.code, envelope.message || "请求失败", response.status);
  }

  return envelope.data || {};
}

export function getJson(path, withCredentials = true) {
  return requestJson("GET", path, null, withCredentials);
}

export function postJson(path, body, withCredentials = true) {
  return requestJson("POST", path, body, withCredentials);
}

