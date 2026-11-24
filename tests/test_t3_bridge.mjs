// tests/test_t3_bridge.mjs
// Summary logic of examples/t3-status/bridge/bridge.mjs (no network).
import assert from "node:assert/strict";
import {
  Detail, describeItem, detailLines, field, keep, shortId, summarize, taskLines,
} from "../examples/t3-status/bridge/bridge.mjs";

const T1 = "2026-11-23T09:12:40.000Z";
const T2 = "2026-11-23T09:55:00.000Z";

// Running and waiting.
let a = keep({ title: "first", status: "running", latestRunCompletedAt: T1 });
let b = keep({ status: "idle", activityRunStatus: "running", pendingRuntimeRequest: { id: "r" } });
let c = keep({ status: "idle", latestRunCompletedAt: null });
assert.deepEqual(summarize([a, b, c]), { run: 2, wait: 1, done_at: Date.parse(T1) / 1000, done_title: "first" });

// A finished run, then a new run that clears latestRunCompletedAt: keep T2.
a = keep({ title: "first", status: "idle", latestRunCompletedAt: T2 }, a);
assert.equal(summarize([a]).done_at, Date.parse(T2) / 1000);
a = keep({ title: "first", status: "running", latestRunCompletedAt: null }, a);
assert.deepEqual(summarize([a]), { run: 1, wait: 0, done_at: Date.parse(T2) / 1000, done_title: "first" });

// Short ids use the random tail, so imported threads stay distinct.
assert.equal(shortId("import:codex:01a13ea4-5a99-7c51-8fa2-7ecab43bea0f"), "b43bea0f");
assert.equal(shortId("2b70e9f3-0ddb-4242-b9df-d8010f172a31"), "0f172a31");

// Fields: control characters and runs of spaces collapse; length in characters.
assert.equal(field("a\tb\n\nc   d ", 10), "a b c d");
assert.equal([...field("\u{1F600}".repeat(70), 60)].length, 60);
assert.equal(field(null, 5), "");

// Task lines: waiting first, then newest start; fields cleaned and cut.
const projects = new Map([["p1", "passport-studio"]]);
const S1 = "2026-11-23T09:41:23.000Z";
const S2 = "2026-11-23T10:06:14.000Z";
const list = [
  keep({ id: "11111111-aaaa", projectId: "p1", title: "older", status: "running", activityRunStartedAt: S1 }),
  keep({ id: "22222222-bbbb", projectId: "p1", title: "new\trun\nline", status: "running", activityRunStartedAt: S2 }),
  keep({ id: "33333333-cccc", projectId: "gone", title: "x".repeat(80), pendingRuntimeRequest: { id: "q" } }),
  keep({ id: "44444444-dddd", projectId: "p1", title: "idle", status: "idle" }),
];
assert.equal(taskLines(list, projects),
  `w\t0\t3333cccc\t\t${"x".repeat(60)}\n` +
  `r\t${Date.parse(S2) / 1000}\t2222bbbb\tpassport-studio\tnew run line\n` +
  `r\t${Date.parse(S1) / 1000}\t1111aaaa\tpassport-studio\tolder\n`);
assert.equal(taskLines([], projects), "");
const many = Array.from({ length: 12 }, (_, i) => keep({ id: `id-${i}`, status: "running" }));
assert.equal(taskLines(many, projects).split("\n").length - 1, 8);

// Describing tool calls.
assert.equal(describeItem({ input: "npm install" }), "npm install");
assert.equal(describeItem({ input: { questions: [{ question: "Flash it?" }, { header: "Port" }] } }), "Flash it? / Port");
assert.equal(describeItem({ input: { command: "ls" } }), "ls");
assert.equal(describeItem({ input: { file_path: "/a/b.c" } }), "/a/b.c");
assert.equal(describeItem({ changes: [{ path: "a.c" }, { path: "b.c" }] }), "a.c, b.c");
assert.equal(describeItem({ title: "Edit", input: null }), "Edit");
assert.equal(describeItem({}), "");

// A thread's detail from its projection and later events.
const d = new Detail();
d.applyProjection({
  messages: [
    { role: "assistant", text: "old reply", createdAt: T1 },
    { role: "user", text: "question", createdAt: T2 },
    { role: "assistant", text: "  ", createdAt: T2 },
  ],
  turnItems: [{ nativeItemRef: { nativeId: "tool1" }, input: "rm -rf build" }],
  runtimeRequests: [
    { id: "req1", status: "pending", nativeRequestRef: { nativeId: "tool1" } },
    { id: "old", status: "resolved", nativeRequestRef: { nativeId: "tool0" } },
  ],
});
assert.equal(d.message.text, "old reply");
assert.equal(d.describeRequest("req1"), "rm -rf build");
assert.equal(d.describeRequest("old"), "");
d.applyEvent({ type: "message.updated", payload: { role: "assistant", text: "new\nreply", createdAt: T2 } });
assert.equal(d.message.text, "new\nreply");
d.applyEvent({ type: "runtime-request.updated", payload: { id: "req1", status: "resolved" } });
assert.equal(d.describeRequest("req1"), "");
d.applyEvent({ type: "unknown", payload: {} });

// Only recent tool calls are kept.
const e = new Detail();
for (let i = 0; i < 100; i++) e.applyItem({ nativeItemRef: { nativeId: `t${i}` }, input: `cmd ${i}` });
assert.equal(e.items.size, 64);
assert.ok(!e.items.has("t0") && e.items.get("t99") === "cmd 99");

// Detail lines.
d.applyEvent({ type: "turn-item.updated", payload: { nativeItemRef: { nativeId: "tool2" }, input: { command: "git push" } } });
d.applyEvent({ type: "runtime-request.updated", payload: { id: "req2", status: "pending", nativeRequestRef: { nativeId: "tool2" } } });
const waiting = keep({
  id: "w", pendingRuntimeRequest: { id: "req2", kind: "command" },
  pendingBackgroundTasks: [{ kind: "command", command: "npm run dev" }, { kind: "subagent", description: "review" }],
});
assert.equal(detailLines(waiting, d), "req\tcommand\tgit push\nmsg\tnew reply\nbg\t2\tnpm run dev / review\n");
// Unknown content, no subscription yet, no background work.
const plain = keep({ id: "p", pendingRuntimeRequest: { id: "zzz", kind: "permission" } });
assert.equal(detailLines(plain, undefined), "req\tpermission\t\n");
assert.equal(detailLines(keep({ id: "r", status: "running" }), new Detail()), "");

console.log("test_t3_bridge: ok");
