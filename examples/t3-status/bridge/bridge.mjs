#!/usr/bin/env node
// T3 Code status bridge for the Passport desk display.
//
// Subscribes to the local T3 Code server with a read-only session token and
// serves a compact summary on the LAN:
//
//   GET /status   -> {"online":true,"run":2,"wait":1,"done_at":1795000000,
//                     "done_title":"Fix login","threads":93}
//   GET /tasks    -> one line per running or waiting thread, waiting first, at
//                    most 8, tab-separated (text/plain, UTF-8):
//                    w|r  started (Unix s)  id (8 chars)  project  title
//   GET /task/<id> -> details of one listed thread, each line optional:
//                    req  <kind>  <what is asked>
//                    msg  <latest assistant reply>
//                    bg   <count>  <first descriptions>
//
// Counts come from orchestration.subscribeShell. Replies and request contents
// are only in a thread's own projection, so the bridge also subscribes
// (orchestration.subscribeThread) to each running or waiting thread and drops
// that subscription when the thread goes idle.
//
// The T3 token never leaves this machine. /tasks and /task show titles,
// replies, and commands to anyone on the LAN; BRIDGE_TASKS=0 turns both off.
//
// Token: issue one with
//   ~/.t3/bin/t3 auth session issue --scope orchestration:read \
//     --label passport-status --ttl 30d --token-only > ~/.config/passport-t3/token
//
// Env: T3_URL (default http://127.0.0.1:3773), T3_TOKEN_FILE, BRIDGE_PORT (8787),
//      BRIDGE_HOST (0.0.0.0), BRIDGE_TASKS (1).
import { readFileSync } from "node:fs";
import { createServer } from "node:http";
import { homedir } from "node:os";
import { join } from "node:path";

const T3_URL = process.env.T3_URL ?? "http://127.0.0.1:3773";
const TOKEN_FILE = process.env.T3_TOKEN_FILE ?? join(homedir(), ".config/passport-t3/token");
const PORT = Number(process.env.BRIDGE_PORT ?? 8787);
const HOST = process.env.BRIDGE_HOST ?? "0.0.0.0";
const TASKS = process.env.BRIDGE_TASKS !== "0";
const STALE_MS = 30_000; // no traffic for this long: reconnect
const MAX_TASKS = 8;
const ITEMS_KEPT = 64; // recent tool calls per thread, to describe a request

// Field lengths in characters; the device's buffers are sized for these.
export const LIMITS = { project: 24, title: 60, doneTitle: 40, request: 80, message: 120, bg: 40 };

const threads = new Map(); // id -> keep()
const projects = new Map(); // id -> title
const details = new Map(); // id -> Detail, for running or waiting threads
let online = false;
let lastMessageAt = 0;
let ws = null;
let backoffMs = 1000;
const unknownKinds = new Set();

function log(...args) {
  console.log(new Date().toISOString(), ...args);
}

function readToken() {
  return readFileSync(TOKEN_FILE, "utf8").trim();
}

// One field: no control characters, at most max characters.
export function field(text, max) {
  return [...String(text ?? "").replace(/[\x00-\x1f\x7f]+/g, " ").replace(/ {2,}/g, " ").trim()]
    .slice(0, max)
    .join("");
}

// Thread ids are UUIDs or "import:<provider>:<uuid>": the tail is the random part.
export function shortId(id) {
  return String(id).replace(/[^A-Za-z0-9]/g, "").slice(-8);
}

// latestRunCompletedAt describes the latest run only: T3 clears it when a new
// run starts, so keep the previous completion until a newer one arrives.
export function keep(t, prev) {
  const completedAt = t.latestRunCompletedAt ? Date.parse(t.latestRunCompletedAt) : 0;
  return {
    id: t.id,
    title: t.title ?? "",
    projectId: t.projectId,
    running: t.status === "running" || t.activityRunStatus === "running",
    waiting: t.pendingRuntimeRequest != null,
    request: t.pendingRuntimeRequest ?? null, // {id, kind, createdAt}
    background: (t.pendingBackgroundTasks ?? []).map((b) => b.description ?? b.command ?? b.kind ?? ""),
    startedAt: Date.parse(t.activityRunStartedAt ?? t.latestRunStartedAt ?? "") || 0,
    completedAt: Math.max(completedAt, prev?.completedAt ?? 0),
  };
}

function upsert(t, location) {
  if (!t?.id) return;
  if (t.deletedAt || t.archivedAt || (location && location !== "active")) threads.delete(t.id);
  else threads.set(t.id, keep(t, threads.get(t.id)));
}

function apply(value) {
  switch (value.kind) {
    case "snapshot":
      // A snapshot may arrive in several chunks (threads, then projects).
      for (const t of value.snapshot?.threads ?? []) upsert(t);
      for (const p of value.snapshot?.projects ?? []) projects.set(p.id, p.title ?? "");
      break;
    case "thread.updated":
      upsert(value.thread, value.location);
      break;
    case "project.updated":
      if (value.project?.id) projects.set(value.project.id, value.project.title ?? "");
      break;
    case "thread.removed":
    case "thread.deleted":
    case "thread.archived":
      threads.delete(value.threadId ?? value.thread?.id);
      break;
    default:
      if (!unknownKinds.has(value.kind)) {
        unknownKinds.add(value.kind);
        log("ignoring event kind", value.kind);
      }
  }
  syncDetails();
}

export function summarize(list) {
  let run = 0;
  let wait = 0;
  let done = null;
  for (const t of list) {
    if (t.running) run++;
    if (t.waiting) wait++;
    if (t.completedAt > (done?.completedAt ?? 0)) done = t;
  }
  return {
    run,
    wait,
    done_at: done ? Math.floor(done.completedAt / 1000) : 0,
    done_title: done ? field(done.title, LIMITS.doneTitle) : "",
  };
}

function active(list) {
  return [...list]
    .filter((t) => t.running || t.waiting)
    .sort((a, b) => b.waiting - a.waiting || b.startedAt - a.startedAt)
    .slice(0, MAX_TASKS);
}

export function taskLines(list, projectTitles) {
  return active(list)
    .map((t) => [
      t.waiting ? "w" : "r",
      Math.floor(t.startedAt / 1000),
      shortId(t.id),
      field(projectTitles.get(t.projectId), LIMITS.project),
      field(t.title, LIMITS.title),
    ].join("\t") + "\n")
    .join("");
}

// ---------------------------------------------------------------------------
// Thread details: latest reply and what a pending request asks for
// ---------------------------------------------------------------------------
export class Detail {
  constructor() {
    this.message = null; // {text, at}
    this.requests = new Map(); // request id -> native tool call id
    this.items = new Map(); // native tool call id -> description, newest last
  }

  // A thread projection (snapshot) or one of its events.
  applyProjection(p) {
    for (const m of p.messages ?? []) this.applyMessage(m);
    for (const i of p.turnItems ?? []) this.applyItem(i);
    for (const r of p.runtimeRequests ?? []) this.applyRequest(r);
  }

  applyEvent(e) {
    if (e?.type === "message.updated") this.applyMessage(e.payload);
    else if (e?.type === "turn-item.updated") this.applyItem(e.payload);
    else if (e?.type === "runtime-request.updated") this.applyRequest(e.payload);
  }

  applyMessage(m) {
    if (m?.role !== "assistant" || !m.text?.trim()) return;
    const at = Date.parse(m.createdAt ?? m.updatedAt ?? "") || 0;
    if (!this.message || at >= this.message.at) this.message = { text: m.text, at };
  }

  applyItem(i) {
    const native = i?.nativeItemRef?.nativeId;
    if (!native) return;
    this.items.delete(native);
    this.items.set(native, describeItem(i));
    if (this.items.size > ITEMS_KEPT) this.items.delete(this.items.keys().next().value);
  }

  applyRequest(r) {
    if (!r?.id) return;
    if (r.status === "pending") this.requests.set(r.id, r.nativeRequestRef?.nativeId ?? null);
    else this.requests.delete(r.id);
  }

  describeRequest(id) {
    const native = this.requests.get(id);
    return native ? this.items.get(native) ?? "" : "";
  }
}

// What a tool call does, in one line: a command, questions, paths, or a title.
export function describeItem(i) {
  const input = i?.input;
  if (typeof input === "string") return input;
  if (input && typeof input === "object") {
    if (Array.isArray(input.questions)) {
      return input.questions.map((q) => q?.question ?? q?.header ?? "").filter(Boolean).join(" / ");
    }
    for (const key of ["command", "cmd", "file_path", "path", "url", "query", "prompt", "description"]) {
      if (typeof input[key] === "string" && input[key]) return input[key];
    }
  }
  for (const key of ["changes", "files"]) {
    const list = Array.isArray(i?.[key]) ? i[key] : Array.isArray(input?.[key]) ? input[key] : null;
    if (list) return list.map((c) => c?.path ?? c?.file_path ?? c).filter((x) => typeof x === "string").join(", ");
  }
  return i?.title ?? "";
}

export function detailLines(thread, detail) {
  let out = "";
  if (thread.request) {
    const text = detail?.describeRequest(thread.request.id) ?? "";
    out += `req\t${field(thread.request.kind, 24) || "other"}\t${field(text, LIMITS.request)}\n`;
  }
  if (detail?.message) out += `msg\t${field(detail.message.text, LIMITS.message)}\n`;
  const bg = thread.background.filter(Boolean);
  if (thread.background.length) {
    out += `bg\t${thread.background.length}\t${field(bg.join(" / "), LIMITS.bg)}\n`;
  }
  return out;
}

// One subscription per running or waiting thread, on its own socket.
function syncDetails() {
  if (!TASKS) return;
  const want = new Set(active(threads.values()).map((t) => t.id));
  for (const [id, d] of details) {
    if (!want.has(id)) {
      details.delete(id);
      d.socket?.close();
    }
  }
  for (const id of want) {
    if (!details.has(id)) {
      const d = new Detail();
      details.set(id, d);
      subscribeThread(id, d);
    }
  }
}

async function openSocket() {
  const res = await fetch(`${T3_URL}/api/auth/websocket-ticket/rpc`, {
    method: "POST",
    headers: { authorization: `Bearer ${readToken()}` },
  });
  if (!res.ok) throw new Error(`ticket HTTP ${res.status}`);
  const { ticket } = await res.json();
  const url = new URL("/ws", T3_URL.replace(/^http/, "ws"));
  url.searchParams.set("orchestrationProtocol", "2");
  url.searchParams.set("wsTicket", ticket);
  return new WebSocket(url);
}

async function subscribeThread(id, d) {
  let sock;
  try {
    sock = await openSocket();
  } catch (err) {
    log("thread detail:", err.message);
    return retryDetail(id, d);
  }
  if (details.get(id) !== d) return sock.close();
  d.socket = sock;
  sock.onopen = () => sock.send(JSON.stringify({
    _tag: "Request", id: "1", tag: "orchestration.subscribeThread",
    payload: { threadId: id, acceptBoundedSnapshot: true }, headers: [],
  }));
  sock.onmessage = (e) => {
    let msg;
    try { msg = JSON.parse(e.data); } catch { return; }
    if (msg._tag === "Ping") return sock.send(JSON.stringify({ _tag: "Pong" }));
    if (msg._tag !== "Chunk") return;
    for (const v of msg.values ?? []) {
      if (v.kind === "snapshot") d.applyProjection(v.projection ?? {});
      else if (v.kind === "event") d.applyEvent(v.event);
    }
    sock.send(JSON.stringify({ _tag: "Ack", requestId: msg.requestId }));
  };
  sock.onclose = () => retryDetail(id, d);
  sock.onerror = () => {};
}

function retryDetail(id, d) {
  if (details.get(id) !== d) return;
  d.socket = null;
  setTimeout(() => details.get(id) === d && subscribeThread(id, d), 5000);
}

// ---------------------------------------------------------------------------
// Shell subscription
// ---------------------------------------------------------------------------
async function connect() {
  let sock;
  try {
    sock = await openSocket();
  } catch (err) {
    log("T3 unreachable:", err.message);
    return scheduleReconnect();
  }
  ws = sock;

  sock.onopen = () => {
    threads.clear();
    projects.clear();
    lastMessageAt = Date.now();
    sock.send(JSON.stringify({
      _tag: "Request", id: "1", tag: "orchestration.subscribeShell", payload: {}, headers: [],
    }));
  };
  sock.onmessage = (e) => {
    lastMessageAt = Date.now();
    let msg;
    try { msg = JSON.parse(e.data); } catch { return; }
    if (msg._tag === "Ping") return sock.send(JSON.stringify({ _tag: "Pong" }));
    if (msg._tag === "Chunk") {
      for (const v of msg.values ?? []) apply(v);
      if (!online) { online = true; backoffMs = 1000; log("subscribed,", threads.size, "threads"); }
      sock.send(JSON.stringify({ _tag: "Ack", requestId: msg.requestId }));
    } else if (msg._tag === "Exit") {
      log("subscription ended:", JSON.stringify(msg.exit ?? msg).slice(0, 200));
      sock.close();
    }
  };
  sock.onclose = (e) => {
    if (ws !== sock) return;
    ws = null;
    online = false;
    log("disconnected", e.code, e.reason || "");
    scheduleReconnect();
  };
  sock.onerror = () => {};
}

function scheduleReconnect() {
  setTimeout(connect, backoffMs);
  backoffMs = Math.min(backoffMs * 2, 30_000);
}

// Keep the connection alive and detect a silent server.
function keepalive() {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  if (Date.now() - lastMessageAt > STALE_MS) {
    log("no traffic, reconnecting");
    ws.close();
    return;
  }
  ws.send(JSON.stringify({ _tag: "Ping" }));
}

function text(res, body) {
  res.writeHead(200, { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" });
  res.end(body);
}

const server = createServer((req, res) => {
  if (req.method !== "GET") return res.writeHead(404).end();
  if (req.url === "/status") {
    const body = { online, ...summarize(threads.values()), threads: threads.size };
    res.writeHead(200, { "content-type": "application/json", "cache-control": "no-store" });
    return res.end(JSON.stringify(body));
  }
  if (TASKS && req.url === "/tasks") return text(res, taskLines(threads.values(), projects));
  const m = TASKS && /^\/task\/([A-Za-z0-9]{1,8})$/.exec(req.url);
  if (m) {
    const t = active(threads.values()).find((x) => shortId(x.id) === m[1]);
    if (!t) return res.writeHead(404).end();
    return text(res, detailLines(t, details.get(t.id)));
  }
  res.writeHead(404).end();
});

if (import.meta.url === `file://${process.argv[1]}`) {
  server.listen(PORT, HOST, () => log(`serving http://${HOST}:${PORT}/status`));
  connect();
  setInterval(keepalive, 10_000);
}
