// SPDX-License-Identifier: Apache-2.0
// Cross-language sproto wire-vector validation for the mistlane generated C#
// bindings: encodes the same locked vectors as the Lua/C++/JS goldens and
// asserts byte-identical output, then decodes them back exactly (including the
// negative wide value the vendored upstream decoder corrupts), and refuses
// malformed wire, invalid UTF-8 and unsupported field kinds.
//
// Owner-run golden (the build requires no .NET toolchain): compile this file
// together with the generated avatar.bindings.cs, e.g.
//   dotnet new console -o cs-golden && cd cs-golden
//   cp <repo>/examples/sproto_sdk_golden.cs Program.cs
//   cp <build>/sproto-fixture/avatar.bindings.cs .
//   dotnet run
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Mistlane.Contracts;

internal static class Program
{
    private static string Hex(byte[] bytes) => string.Concat(bytes.Select(b => b.ToString("x2")));
    private static byte[] Unhex(string text)
    {
        var bytes = new byte[text.Length / 2];
        for (int i = 0; i < bytes.Length; i++) bytes[i] = Convert.ToByte(text.Substring(i * 2, 2), 16);
        return bytes;
    }
    private static void ThrowOn(bool bad, string label)
    {
        if (bad) throw new InvalidOperationException(label + " mismatch");
    }
    private static void AssertVectors()
    {
        var vectors = new (string Label, IReadOnlyDictionary<string, object> Value, string Expected)[]
        {
            ("level=1", new Dictionary<string, object> { ["level"] = 1L }, "020001000400"),
            ("int64_negative", new Dictionary<string, object> { ["exp"] = -9007199254740993L }, "02000700000008000000ffffffffffffdfff"),
            ("int64_positive", new Dictionary<string, object> { ["level"] = 65535L, ["exp"] = 9007199254740993L }, "0400010000000300000004000000ffff0000080000000100000000002000"),
            ("nested_struct", new Dictionary<string, object> {
                ["inventory"] = new Dictionary<string, object> {
                    ["capacity"] = 1L,
                    ["items"] = new List<Dictionary<string, object>> {
                        new Dictionary<string, object> { ["itemId"] = 7L, ["count"] = 2L },
                        new Dictionary<string, object> { ["itemId"] = 0L, ["count"] = 1L },
                    },
                },
            }, "0200030000001e000000020004000000140000000600000002001000060006000000020002000400"),
            ("int_array32", new Dictionary<string, object> { ["level"] = 1L, ["tags"] = new List<long> { 0L, 1L, 2147483647L } }, "040001000400050000000d000000040000000001000000ffffff7f"),
            ("int_array64", new Dictionary<string, object> { ["totals"] = new List<long> { -1L, 2147483648L, 4294967296L } }, "02000b0000001900000008ffffffffffffffff00000080000000000000000001000000"),
            ("empty_int_array", new Dictionary<string, object> { ["tags"] = new List<long>() }, "02000900000000000000"),
            ("empty_struct_array", new Dictionary<string, object> { ["inventory"] = new Dictionary<string, object> { ["capacity"] = 0L, ["items"] = new List<Dictionary<string, object>>() } }, "0200030000000a00000002000200000000000000"),
            ("bool_string_bytes", new Dictionary<string, object> {
                ["active"] = true,
                ["name"] = "中文",
                ["blob"] = new byte[] { 0, 255 },
                ["flags"] = new List<bool> { true, false, true },
                ["aliases"] = new List<string> { "latin", "" },
                ["blobs"] = new List<byte[]> { new byte[] { 1, 2 }, new byte[0] },
            }, "07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000"),
            ("bool_false", new Dictionary<string, object> { ["active"] = false }, "02000d000200"),
            ("all_empty", new Dictionary<string, object> {
                ["name"] = "", ["blob"] = new byte[0],
                ["flags"] = new List<bool>(), ["aliases"] = new List<string>(), ["blobs"] = new List<byte[]>(),
            }, "06000f00000000000000000000000000000000000000000000000000000000000000"),
        };
        foreach (var (label, value, expected) in vectors)
        {
            var encoded = SprotoCodec.Encode(AvatarSchema.Fields, value);
            if (Hex(encoded) != expected) throw new InvalidOperationException($"CS sproto vector {label}: {Hex(encoded)} != {expected}");
            Console.WriteLine($"CS_SPROTO_SDK_GOLDEN {label} hex={Hex(encoded)} [PASS]");
            var decoded = SprotoCodec.Decode(AvatarSchema.Fields, encoded);
            if (Hex(SprotoCodec.Encode(AvatarSchema.Fields, decoded)) != expected) throw new InvalidOperationException($"CS sproto roundtrip {label} changed the bytes");
        }
    }
    private static void AssertDecode()
    {
        var negative = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("02000700000008000000ffffffffffffdfff"));
        ThrowOn((long)negative["exp"] != -9007199254740993L, "int64_negative decode");
        var nested = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("0200030000001e000000020004000000140000000600000002001000060006000000020002000400"));
        var inventory = (Dictionary<string, object>)nested["inventory"];
        var items = (List<Dictionary<string, object>>)inventory["items"];
        ThrowOn((long)inventory["capacity"] != 1L, "nested capacity");
        ThrowOn((long)items[0]["itemId"] != 7L || (long)items[0]["count"] != 2L, "nested item0");
        ThrowOn((long)items[1]["itemId"] != 0L || (long)items[1]["count"] != 1L, "nested item1");
        var totals = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("02000b0000001900000008ffffffffffffffff00000080000000000000000001000000"));
        var list = (List<long>)totals["totals"];
        ThrowOn(list[0] != -1L || list[1] != 2147483648L || list[2] != 4294967296L, "int array64 decode");
        var text = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("07000d0004000000000000000000000006000000e4b8ade696870200000000ff030000000100010d000000050000006c6174696e000000000a00000002000000010200000000"));
        var flags = (List<bool>)text["flags"];
        var aliases = (List<string>)text["aliases"];
        var blobs = (List<byte[]>)text["blobs"];
        ThrowOn(!Equals(text["active"], true) || (string)text["name"] != "中文"
            || !((byte[])text["blob"]).SequenceEqual(new byte[] { 0, 255 }), "bool/string/bytes decode");
        ThrowOn(flags.Count != 3 || !flags[0] || flags[1] || !flags[2]
            || aliases[0] != "latin" || aliases[1] != ""
            || !blobs[0].SequenceEqual(new byte[] { 1, 2 }) || blobs[1].Length != 0, "bool/string/bytes arrays decode");
        var allEmpty = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("06000f00000000000000000000000000000000000000000000000000000000000000"));
        ThrowOn((string)allEmpty["name"] != "" || ((byte[])allEmpty["blob"]).Length != 0
            || ((List<bool>)allEmpty["flags"]).Count != 0 || ((List<string>)allEmpty["aliases"]).Count != 0
            || ((List<byte[]>)allEmpty["blobs"]).Count != 0, "empty string/arrays decode");
        var unknown = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("020019000200"));
        ThrowOn(unknown.Count != 0, "unknown tag skipped");
        var missing = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("020001000400"));
        ThrowOn((long)missing["level"] != 1L || missing.ContainsKey("exp"), "absent field decoded as present");
        Console.WriteLine("CS_SPROTO_SDK_DECODE scalar+struct+arrays+bool/string/bytes roundtrip unknown_tag=skipped absent=missing [PASS]");
    }
    private static void AssertMalformed()
    {
        void Throws(string label, byte[] bytes)
        {
            try { SprotoCodec.Decode(AvatarSchema.Fields, bytes); }
            catch (InvalidDataException) { return; }
            catch (Exception error) { throw new InvalidOperationException(label + ": unexpected error class " + error.GetType().Name); }
            throw new InvalidOperationException(label + " decoded instead of throwing");
        }
        Throws("short header", Unhex("02"));
        Throws("short tag block", Unhex("03000100"));
        Throws("truncated value", Unhex("02000700000008000000ff"));
        Throws("trailing bytes", Unhex("02000100040000"));
        Throws("bad array layout", Unhex("02000900000003000000"));
        // An invalid UTF-8 STRING payload, a boolean-array byte that is neither
        // 0 nor 1, and a string-array element whose length prefix overruns the
        // array content all fail closed.
        Throws("invalid utf8 string", Unhex("02000f00000001000000ff"));
        Throws("bad boolean array", Unhex("020013000000010000000002"));
        Throws("bad string-array element", Unhex("020015000000050000000500000061"));
        Console.WriteLine("CS_SPROTO_SDK_DECODE malformed: short/truncated/array-layout/utf-8/bool rejected [PASS]");
    }
    private static void AssertEncodeRejections()
    {
        void Throws(string label, IReadOnlyDictionary<string, object> value)
        {
            try { SprotoCodec.Encode(AvatarSchema.Fields, value); }
            catch (InvalidDataException) { return; }
            catch (Exception error) { throw new InvalidOperationException(label + ": unexpected error class " + error.GetType().Name); }
            throw new InvalidOperationException(label + " encoded instead of throwing");
        }
        Throws("non-integer", new Dictionary<string, object> { ["level"] = 1.5 });
        Throws("not a struct", new Dictionary<string, object> { ["inventory"] = 5L });
        Throws("not an array", new Dictionary<string, object> { ["tags"] = 5L });
        Throws("not a struct array", new Dictionary<string, object> {
            ["inventory"] = new Dictionary<string, object> { ["capacity"] = 1L, ["items"] = "x" },
        });
        Throws("not a boolean", new Dictionary<string, object> { ["active"] = "yes" });
        Throws("not a string", new Dictionary<string, object> { ["name"] = 5L });
        Throws("not bytes", new Dictionary<string, object> { ["blob"] = "x" });
        Throws("not a boolean array", new Dictionary<string, object> { ["flags"] = new List<string> { "yes" } });
        // A kind outside the def grammar still cannot slip through: an injected
        // unknown kind is refused on both directions (the generated tables only
        // carry known kinds, so this pins the defensive default).
        void ThrowsFields(string label, SprotoField[] fields, IReadOnlyDictionary<string, object> value)
        {
            try { SprotoCodec.Encode(fields, value); }
            catch (InvalidDataException) { return; }
            catch (Exception error) { throw new InvalidOperationException(label + ": unexpected error class " + error.GetType().Name); }
            throw new InvalidOperationException(label + " encoded instead of throwing");
        }
        void ThrowsDecode(string label, SprotoField[] fields, byte[] bytes)
        {
            try { SprotoCodec.Decode(fields, bytes); }
            catch (InvalidDataException) { return; }
            catch (Exception error) { throw new InvalidOperationException(label + ": unexpected error class " + error.GetType().Name); }
            throw new InvalidOperationException(label + " decoded instead of throwing");
        }
        // id 0 so the in-band/length-prefixed wire tags below reach the field.
        var unsupported = new SprotoField(0, "zz", SprotoKind.Unsupported);
        ThrowsFields("unsupported kind", new SprotoField[] { unsupported }, new Dictionary<string, object> { ["zz"] = true });
        ThrowsDecode("unsupported in-band decode", new SprotoField[] { unsupported }, Unhex("01000200"));
        ThrowsDecode("unsupported length-prefixed decode", new SprotoField[] { unsupported }, Unhex("0100000000000000"));
        Console.WriteLine("CS_SPROTO_SDK_ENCODE non-integer/shape/unsupported-kind rejected [PASS]");
    }
    private static void AssertEnvelope()
    {
        // The invocation envelope and typed reply (docs/entity-contract.md):
        // the C++ server bytes are the reference (the entity.envelope case pins
        // them). The generated client codec encodes the same fixed big-endian
        // header byte for byte — the canonical vector, then the extremes with
        // a negative entity id and the maximum correlation — decodes them
        // exactly, and fails closed on truncation, unknown side bytes and
        // malformed replies (kind, code, reason length, UTF-8, trailing).
        void Throws(string label, Func<byte[]> encode)
        {
            try { encode(); }
            catch (InvalidDataException) { return; }
            catch (Exception error) { throw new InvalidOperationException(label + ": unexpected error class " + error.GetType().Name); }
            throw new InvalidOperationException(label + " encoded instead of throwing");
        }
        var canonical = new EntityEnvelope(7, 1, 42, 3, 1, 2, 0x1122334455667788UL, 5, 250,
            Encoding.UTF8.GetBytes("payload"));
        var canonicalBytes = Unhex("0000000700000001000000000000002a00000003010002112233445566778800000005000000fa7061796c6f6164");
        if (!EntityEnvelopeCodec.Encode(canonical).SequenceEqual(canonicalBytes))
            throw new InvalidOperationException("envelope canonical bytes drifted");
        var decoded = EntityEnvelopeCodec.Decode(canonicalBytes);
        if (decoded == null || decoded.Value.Scope != 7 || decoded.Value.EntityId != 42 ||
            decoded.Value.Incarnation != 3 || decoded.Value.Side != 1 || decoded.Value.MethodId != 2 ||
            decoded.Value.CorrelationId != 0x1122334455667788UL || decoded.Value.HostingGeneration != 5 ||
            decoded.Value.DeadlineMs != 250 || !decoded.Value.Payload.SequenceEqual(canonical.Payload))
            throw new InvalidOperationException("envelope decode lost fields");
        var extremeBytes = Unhex("ffffffff0000003ffffffffffffffffdffffffff0000fefffffffffffffffffffffffeffffffff");
        var extreme = EntityEnvelopeCodec.Decode(extremeBytes);
        if (extreme == null || extreme.Value.EntityId != -3 || extreme.Value.CorrelationId != ulong.MaxValue)
            throw new InvalidOperationException("envelope signed entity id or unsigned correlation lost");
        if (!EntityEnvelopeCodec.Encode(extreme.Value).SequenceEqual(extremeBytes))
            throw new InvalidOperationException("envelope extreme re-encode drifted");
        if (EntityEnvelopeCodec.Decode(new byte[38]) != null)
            throw new InvalidOperationException("truncated envelope decoded");
        var badSide = (byte[])canonicalBytes.Clone();
        badSide[20] = 2;
        if (EntityEnvelopeCodec.Decode(badSide) != null)
            throw new InvalidOperationException("unknown side byte decoded");
        Throws("envelope side range", () => EntityEnvelopeCodec.Encode(new EntityEnvelope(0, 1, 0, 1, 2, 0, 0, 0, 0, Array.Empty<byte>())));
        // Reply shape: one of returns or a typed error, never both.
        if (!EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(Encoding.UTF8.GetBytes("returns"), null, "")).SequenceEqual(Unhex("0072657475726e73")))
            throw new InvalidOperationException("ok reply drifted");
        if (!EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(Array.Empty<byte>(), "INVALID", "bad shape")).SequenceEqual(Unhex("01000009626164207368617065")))
            throw new InvalidOperationException("INVALID reply drifted");
        if (!EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(Array.Empty<byte>(), "STALE", "")).SequenceEqual(Unhex("0102")))
            throw new InvalidOperationException("STALE reply drifted");
        var replyOk = EntityEnvelopeCodec.DecodeReply(Unhex("0072657475726e73"));
        if (replyOk == null || replyOk.Value.Error != null || !replyOk.Value.Payload.SequenceEqual(Encoding.UTF8.GetBytes("returns")))
            throw new InvalidOperationException("ok reply decode failed");
        var replyInvalid = EntityEnvelopeCodec.DecodeReply(Unhex("01000009626164207368617065"));
        if (replyInvalid == null || replyInvalid.Value.Error != "INVALID" || replyInvalid.Value.Reason != "bad shape")
            throw new InvalidOperationException("INVALID reply reason lost");
        var replyStale = EntityEnvelopeCodec.DecodeReply(Unhex("0102"));
        if (replyStale == null || replyStale.Value.Error != "STALE")
            throw new InvalidOperationException("STALE reply decode failed");
        if (EntityEnvelopeCodec.DecodeReply(Array.Empty<byte>()) != null)
            throw new InvalidOperationException("empty reply decoded");
        if (EntityEnvelopeCodec.DecodeReply(Unhex("0578")) != null)
            throw new InvalidOperationException("unknown reply kind decoded");
        if (EntityEnvelopeCodec.DecodeReply(Unhex("01c8")) != null)
            throw new InvalidOperationException("unknown error code decoded");
        if (EntityEnvelopeCodec.DecodeReply(Unhex("01")) != null)
            throw new InvalidOperationException("truncated reply decoded");
        if (EntityEnvelopeCodec.DecodeReply(Unhex("010000056162")) != null)
            throw new InvalidOperationException("reason length mismatch decoded");
        if (EntityEnvelopeCodec.DecodeReply(Unhex("010200")) != null)
            throw new InvalidOperationException("trailing bytes after STALE decoded");
        if (EntityEnvelopeCodec.DecodeReply(Unhex("01000002ffff")) != null)
            throw new InvalidOperationException("invalid utf-8 reason decoded");
        Throws("reply reason on non-INVALID", () => EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(Array.Empty<byte>(), "STALE", "x")));
        Throws("reply unknown error", () => EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(Array.Empty<byte>(), "NOPE", "")));
        Throws("reply reason too long", () => EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(Array.Empty<byte>(), "INVALID", new string('a', 65536))));
        Console.WriteLine("CS_ENTITY_ENVELOPE canonical/extremes/replies and malformed refusals [PASS]");
    }
    private static void AssertFacade()
    {
        // Client-side bounded queue and the direct Avatar facade (the C#
        // mirror of the JS golden): typed admissions, deadlines on an injected
        // clock, surfaced timeout tombstones, retry only for idempotent calls
        // with a fresh correlation id, and envelope bytes straight off the
        // generated method tables — no .server/.client intermediate layer.
        var q = new EntityRpcQueue(2);
        if (q.Call(100, false, null, 7) != 7 || q.Call(100, false, null, 8) != 8)
            throw new InvalidOperationException("queue admission");
        if (q.Call(100, false, null, 7) != 0 || q.Error != "INVALID") throw new InvalidOperationException("duplicate not INVALID");
        if (q.Call(100) != 0 || q.Error != "CAPACITY") throw new InvalidOperationException("overflow not CAPACITY");
        if (q.Call(0, false, null, 9) != 0 || q.Error != "INVALID") throw new InvalidOperationException("zero deadline not INVALID");
        if (!q.Advance(-1) == false) throw new InvalidOperationException("negative clock accepted");
        q.Advance(100);
        if (q.State(7) != "TIMED_OUT" || q.State(8) != "TIMED_OUT") throw new InvalidOperationException("deadlines not burned");
        if (q.Complete(7) != "TIMEOUT") throw new InvalidOperationException("late reply not TIMEOUT");
        if (q.Complete(99) != "STALE") throw new InvalidOperationException("unknown reply not STALE");
        var tomb = q.Take(7);
        if (tomb == null || tomb.State != "TIMED_OUT" || tomb.Idempotent) throw new InvalidOperationException("tombstone not surfaced");
        if (q.Size != 1) throw new InvalidOperationException("take did not release the slot");
        var cancelled = q.Call(50);
        if (!q.Cancel(cancelled) || q.Cancel(cancelled) || q.Error != "INVALID") throw new InvalidOperationException("cancel discipline");
        if (q.Complete(cancelled) != "CANCELLED") throw new InvalidOperationException("reply after cancel");
        q.Take(cancelled);
        var doubleReply = q.Call(100);
        if (q.Complete(doubleReply) != "OK" || q.Complete(doubleReply) != "CONFLICT") throw new InvalidOperationException("double reply not CONFLICT");
        q.Take(doubleReply);
        q.Call(100, false, null, 21);
        q.Advance(200);
        if (q.Retry(21, 300) != null) throw new InvalidOperationException("non-idempotent timeout retried");
        if (q.State(21) != "TIMED_OUT") throw new InvalidOperationException("tombstone vanished after refused retry");
        q.Take(8);  // free one slot; the table below stays at capacity through the retry
        if (q.Call(100, true, null, 31) != 31) throw new InvalidOperationException("idempotent admission refused");
        q.Advance(400);
        var fresh = q.Retry(31, 300);  // full table: the tombstone slot is reused
        if (fresh == null || fresh == 31 || q.State(31) != null
            || q.State(fresh.Value) != "PENDING" || !q.Get(fresh.Value).Idempotent)
            throw new InvalidOperationException("idempotent re-arm");
        var sent = new List<byte[]>();
        var client = new EntityClient(sent.Add, "Avatar", 42, 3, 0, 5000, 2);
        if (client.Call("nope").Ok || client.Call("attack").Ok || client.Notify("getProfile").Ok)
            throw new InvalidOperationException("facade mode misuse not INVALID");
        var call = client.Call("getProfile", new Dictionary<string, object> { ["account"] = 9L }, 100, true);
        if (!call.Ok) throw new InvalidOperationException("call refused");
        var env = EntityEnvelopeCodec.Decode(sent[0]);
        if (env == null || env.Value.Side != 0 || env.Value.MethodId != 2 || env.Value.CorrelationId != (ulong)call.Id
            || env.Value.HostingGeneration != 0 || env.Value.DeadlineMs != 100 || env.Value.EntityId != 42)
            throw new InvalidOperationException("facade envelope wrong");
        if (!env.Value.Payload.SequenceEqual(SprotoCodec.Encode(ContractSchemas.Types["Avatar_getProfileArgs"],
            new Dictionary<string, object> { ["account"] = 9L })))
            throw new InvalidOperationException("args payload wrong");
        var second = client.Call("getProfile", new Dictionary<string, object> { ["account"] = 10L }, 100);
        if (!second.Ok) throw new InvalidOperationException("second call refused");
        var overflow = client.Call("getProfile", new Dictionary<string, object> { ["account"] = 11L });
        if (overflow.Ok || overflow.Error != "CAPACITY") throw new InvalidOperationException("facade overflow not typed");
        var returns = EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(
            SprotoCodec.Encode(ContractSchemas.Types["Avatar_getProfileReturns"], new Dictionary<string, object> { ["level"] = 7L }), null, ""));
        if (client.OnReply(call.Id, returns) != "OK") throw new InvalidOperationException("reply verdict");
        var done = client.Take(call.Id);
        if (done == null || done.State != "OK" || done.Error != null || Convert.ToInt64(done.Response["level"]) != 7)
            throw new InvalidOperationException("typed return lost");
        // a typed error settles the call: the verdict is OK, the entry carries STALE
        if (client.OnReply(second.Id, EntityEnvelopeCodec.EncodeReply(new EntityEnvelopeCodec.EntityReply(
                Array.Empty<byte>(), "STALE", ""))) != "OK")
            throw new InvalidOperationException("error reply verdict");
        var settled = client.Take(second.Id);
        if (settled == null || settled.Error != "STALE" || settled.Response != null)
            throw new InvalidOperationException("typed error lost");
        var retryable = client.Call("getProfile", new Dictionary<string, object> { ["account"] = 12L }, 50, true);
        client.Advance(1000);
        var first = EntityEnvelopeCodec.Decode(sent[sent.Count - 1]);
        var resent = client.Retry(retryable.Id, 200);
        if (resent == null || resent == retryable.Id) throw new InvalidOperationException("retry not fresh");
        var resentEnv = EntityEnvelopeCodec.Decode(sent[sent.Count - 1]);
        if (resentEnv.Value.CorrelationId != (ulong)resent.Value || resentEnv.Value.MethodId != 2
            || !resentEnv.Value.Payload.SequenceEqual(first.Value.Payload))
            throw new InvalidOperationException("retry envelope wrong");
        client.Advance(1200);
        if (client.OnReply(resent.Value, returns) != "TIMEOUT") throw new InvalidOperationException("late reply after deadline");
        var pending = client.Call("getProfile", new Dictionary<string, object> { ["account"] = 13L });
        if (client.OnReply(pending.Id, Unhex("0578")) != "MALFORMED" || client.Queue.State(pending.Id) != "PENDING")
            throw new InvalidOperationException("malformed reply consumed the slot");
        string noticedName = null;
        IReadOnlyDictionary<string, object> noticedArgs = null;
        var notifyBytes = EntityEnvelopeCodec.Encode(new EntityEnvelope(0, 1, -1, 0, 1, 1, 0, 0, 0,
            SprotoCodec.Encode(ContractSchemas.Types["Avatar_onRewardArgs"], new Dictionary<string, object> { ["itemId"] = 77L })));
        client.OnNotify(notifyBytes, (name, args) => { noticedName = name; noticedArgs = args; return null; });
        if (noticedName != "onReward" || Convert.ToUInt32(noticedArgs["itemId"]) != 77U)
            throw new InvalidOperationException("inbound notify lost");
        if (client.OnNotify(EntityEnvelopeCodec.Encode(new EntityEnvelope(0, 1, 42, 0, 0, 1, 0, 0, 0, Array.Empty<byte>())),
                (name, args) => throw new InvalidOperationException("client-side frame delivered as notify")) != null
            || client.OnNotify(Unhex("0578"),
                (name, args) => throw new InvalidOperationException("malformed frame delivered as notify")) != null)
            throw new InvalidOperationException("inbound notify accepted a client-side or malformed frame");
        Console.WriteLine("CS_ENTITY_CLIENT bounded queue, facade calls/notify/retry and malformed refusals [PASS]");
    }
    private static Dictionary<string, object> PlainBaseline() => new Dictionary<string, object> {
        ["level"] = 7L, ["tags"] = new List<long> { 1L, 2L }, ["score"] = 100L, ["active"] = true, ["name"] = "rei",
    };
    private static Dictionary<string, object> InventoryBaseline() => new Dictionary<string, object> {
        ["exp"] = 40L, ["tags"] = new List<long> { 1L, 2L },
        ["inventory"] = new Dictionary<string, object> { ["capacity"] = 10L,
            ["items"] = new List<object> { new Dictionary<string, object> { ["itemId"] = 1L, ["count"] = 2L } } },
    };
    private static void AssertSync()
    {
        // Client property replica: the PropertySync mirror of the C++
        // SyncReceiver — descriptor-driven baselines with defaults, gapless
        // sequences, atomic batch rollback, stale-until-resync, and handlers
        // that fire only for subscribed properties after the commit.
        var seen = new List<string>();
        var sync = new PropertySync("Avatar", new Dictionary<string, Action<PropertyEvent>> {
            ["exp"] = e => seen.Add("exp:" + e.Kind + ":" + e.Value),
            ["tags"] = e => seen.Add("tags:" + e.Kind + ":" + e.Position),
        });
        sync.Baseline(PlainBaseline());
        if (sync.Sequence != 0 || sync.Stale) throw new InvalidOperationException("baseline does not reset state");
        if (!Equals(sync.Get("exp"), 0L) || !(sync.Get("blob") is byte[]))
            throw new InvalidOperationException("missing properties not defaulted");
        if (((List<long>)sync.Get("tags"))[0] != 1L) throw new InvalidOperationException("baseline values not stored");
        var original = sync.Get("tags");
        if (!sync.Apply(new List<SyncDelta> { new(4, Array.Empty<SyncStep>(), "SET", 0, 99L, 1) })
            || !Equals(sync.Get("exp"), 99L) || sync.Sequence != 1)
            throw new InvalidOperationException("SET delta not applied");
        if (seen[0] != "exp:SET:99") throw new InvalidOperationException("handler not fired in order");
        if (sync.Apply(new List<SyncDelta> { new(4, Array.Empty<SyncStep>(), "SET", 0, 5L, 3) })
            || !sync.Stale || !Equals(sync.Get("exp"), 99L))
            throw new InvalidOperationException("sequence gap not refused with rollback");
        if (sync.Apply(new List<SyncDelta> { new(4, Array.Empty<SyncStep>(), "SET", 0, 6L, 2) }))
            throw new InvalidOperationException("stale receiver accepted work");
        sync.Baseline(InventoryBaseline());
        if (sync.Sequence != 0 || sync.Stale || !Equals(sync.Get("exp"), 40L))
            throw new InvalidOperationException("baseline does not resync");
        if (ReferenceEquals(sync.Get("tags"), original)) throw new InvalidOperationException("baseline aliased the caller list");
        if (sync.Apply(new List<SyncDelta> {
                new(4, Array.Empty<SyncStep>(), "SET", 0, 77L, 1),
                new(99, Array.Empty<SyncStep>(), "SET", 0, 1L, 2),
            }) || !sync.Stale || !Equals(sync.Get("exp"), 40L))
            throw new InvalidOperationException("batch with unknown property not rolled back atomically");
        sync.Baseline(InventoryBaseline());
        if (!sync.Apply(new List<SyncDelta> {
                new(5, Array.Empty<SyncStep>(), "INSERT", 0, 9L, 1),
                new(5, Array.Empty<SyncStep>(), "REMOVE", 1, null, 2),
            }) || ((List<long>)sync.Get("tags"))[0] != 9L || ((List<long>)sync.Get("tags"))[1] != 2L
            || ((List<long>)sync.Get("tags")).Count != 2)
            throw new InvalidOperationException("array insert/remove not applied");
        if (sync.Apply(new List<SyncDelta> { new(5, Array.Empty<SyncStep>(), "INSERT", 5, 9L, 3) })
            || !sync.Stale || ((List<long>)sync.Get("tags")).Count != 2)
            throw new InvalidOperationException("impossible insert position not refused");
        sync.Baseline(InventoryBaseline());
        if (sync.Apply(new List<SyncDelta> {
                new(4, new[] { new SyncStep(FieldId: 0) }, "SET", 0, "yes", 1),
            }) || !sync.Stale)
            throw new InvalidOperationException("non-struct path into int property accepted");
        sync.Baseline(InventoryBaseline());
        if (sync.Apply(new List<SyncDelta> { new(7, Array.Empty<SyncStep>(), "SET", 0, "yes", 1) })
            || !sync.Stale || Equals(sync.Get("active"), "yes"))
            throw new InvalidOperationException("wrong value type not refused with rollback");
        sync.Baseline(InventoryBaseline());
        if (!sync.Apply(new List<SyncDelta> { new(3, Array.Empty<SyncStep>(), "SET", 0, 11L, 1) }))
            throw new InvalidOperationException("unsubscribed property SET refused");
        if (seen.Count != 3) throw new InvalidOperationException("unsubscribed handler fired");
        if (!sync.Apply(new List<SyncDelta> {
                new(2, new[] { new SyncStep(FieldId: 0) }, "SET", 0, 30L, 2),
                new(2, new[] { new SyncStep(FieldId: 1) }, "INSERT", 0,
                    new Dictionary<string, object> { ["itemId"] = 5L, ["count"] = 1L }, 3),
            }))
            throw new InvalidOperationException("struct path batch refused");
        var inv = (Dictionary<string, object>)sync.Get("inventory");
        var items = (List<object>)inv["items"];
        if (!Equals(inv["capacity"], 30L) || items.Count != 2
            || !Equals(((Dictionary<string, object>)items[0])["itemId"], 5L))
            throw new InvalidOperationException("struct path set and struct-array insert failed");
        if (sync.Apply(new List<SyncDelta> {
                new(2, new[] { new SyncStep(FieldId: 77) }, "SET", 0, 1L, 4),
            }) || !sync.Stale || !Equals(inv["capacity"], 30L))
            throw new InvalidOperationException("unknown struct field not refused with rollback");
        try { new PropertySync("Nope"); throw new InvalidOperationException("unknown entity type accepted"); }
        catch (InvalidDataException) { }
        if (string.Join("|", seen) != "exp:SET:99|tags:INSERT:0|tags:REMOVE:1")
            throw new InvalidOperationException("unexpected handler stream");
        Console.WriteLine("CS_PROPERTY_SYNC baseline defaults, delta batches, atomic rollback and resync [PASS]");
    }
    public static int Main()
    {
        AssertVectors();
        AssertDecode();
        AssertMalformed();
        AssertEncodeRejections();
        AssertEnvelope();
        AssertFacade();
        AssertSync();
        Console.WriteLine("CS_SPROTO_SDK_OK scalar+struct+arrays+bool/string/bytes vectors and decode match the locked bytes");
        return 0;
    }
}
