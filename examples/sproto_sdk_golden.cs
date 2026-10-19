// SPDX-License-Identifier: Apache-2.0
// Cross-language sproto wire-vector validation for the mistlane generated C#
// bindings: encodes the same locked vectors as the Lua/C++/JS goldens and
// asserts byte-identical output, then decodes them back exactly (including the
// negative wide value the vendored upstream decoder corrupts), and refuses
// malformed wire and unsupported field kinds.
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
        var unknown = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("02000d000200"));
        ThrowOn(unknown.Count != 0, "unknown tag skipped");
        var missing = SprotoCodec.Decode(AvatarSchema.Fields, Unhex("020001000400"));
        ThrowOn((long)missing["level"] != 1L || missing.ContainsKey("exp"), "absent field decoded as present");
        Console.WriteLine("CS_SPROTO_SDK_DECODE scalar+struct+arrays roundtrip unknown_tag=skipped absent=missing [PASS]");
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
        Console.WriteLine("CS_SPROTO_SDK_DECODE malformed: short/truncated/array-layout rejected [PASS]");
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
        // Unsupported kinds are emitted into the tables (BOOL/STRING/BYTES until
        // their wire forms land) but the codec refuses them on both directions.
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
        ThrowsDecode("unsupported length-prefixed decode", new SprotoField[] { unsupported }, Unhex("01000000"));
        Console.WriteLine("CS_SPROTO_SDK_ENCODE non-integer/shape/unsupported-kind rejected [PASS]");
    }
    public static int Main()
    {
        AssertVectors();
        AssertDecode();
        AssertMalformed();
        AssertEncodeRejections();
        Console.WriteLine("CS_SPROTO_SDK_OK scalar+struct+arrays vectors and decode match the locked bytes");
        return 0;
    }
}
