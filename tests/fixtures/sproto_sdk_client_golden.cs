// SPDX-License-Identifier: Apache-2.0
// Client-audience artifact golden for the MLV1 session negotiation surface
// (docs/entity-contract.md, entity.client.versions), mirroring
// sproto_sdk_client_golden.mjs: the digest the generated client emits must
// agree with the published lock written beside it, the handshake frame bytes
// are pinned, every host reply verdict parses, and malformed replies refuse
// closed. A small codec probe confirms the client artifact kept the wire
// codec its server twin pins in sproto_sdk_golden.cs.
//
// Owner-run golden (the build requires no .NET toolchain): compile this file
// together with the generated avatar.client.cs, e.g.
//   dotnet new console -o cs-golden && cd cs-golden
//   cp <repo>/tests/fixtures/sproto_sdk_client_golden.cs Program.cs
//   cp <build>/sproto-fixture/avatar.client.cs .
//   cp <build>/sproto-fixture/avatar.lock .
//   dotnet run
using System;
using System.IO;
using System.Linq;
using System.Text;
using Mistlane.Contracts;

internal static class Program
{
    private static string Hex(byte[] bytes) => string.Concat(bytes.Select(b => b.ToString("x2")));
    private static byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text);
    private static bool BytesEqual(byte[] a, byte[] b) => a.SequenceEqual(b);
    private static void ThrowOn(bool bad, string label)
    {
        if (bad) throw new InvalidOperationException(label + " mismatch");
    }

    private static void Main()
    {
        // The audience filter must not have touched the wire codec: the
        // canonical envelope vector and one sproto vector are the same bytes
        // the server artifact's golden pins.
        var canonical = EntityEnvelopeCodec.Encode(new EntityEnvelope(7, 1, 42, 3, 1, 2,
          0x1122334455667788UL, 5, 250, Utf8("payload")));
        ThrowOn(Hex(canonical) != "0000000700000001000000000000002a00000003010002112233445566778800000005000000fa7061796c6f6164",
          "client artifact envelope bytes");
        ThrowOn(Hex(SprotoCodec.Encode(ContractSchemas.Types["Avatar"],
          new System.Collections.Generic.Dictionary<string, object> { ["level"] = 1L })) != "020001000400",
          "client artifact sproto bytes");
        Console.WriteLine("CS_MLV1_CLIENT_CODEC envelope and sproto bytes byte-identical to the server artifact [PASS]");

        // The digest is derived from the compiled contract, so it must equal
        // the digest line the published lock pins.
        ThrowOn(!System.Text.RegularExpressions.Regex.IsMatch(Mlv1.ContractDigest, "^[0-9a-f]{16}$"),
          "CONTRACT_DIGEST shape");
        var lockDigest = File.ReadAllLines("avatar.lock")
          .FirstOrDefault(line => line.StartsWith("# digest ", StringComparison.Ordinal));
        ThrowOn(lockDigest == null, "lock digest line");
        lockDigest = lockDigest.Substring("# digest ".Length).Trim();
        ThrowOn(Mlv1.ContractDigest != lockDigest,
          $"CONTRACT_DIGEST {Mlv1.ContractDigest} vs lock digest {lockDigest}");
        Console.WriteLine($"CS_MLV1_CLIENT_DIGEST {Mlv1.ContractDigest} agrees with avatar.lock [PASS]");

        // The frame is exactly "MLV1 <digest>".
        ThrowOn(!BytesEqual(Mlv1.HandshakeFrame(), Utf8("MLV1 " + Mlv1.ContractDigest)),
          $"handshake frame bytes {Hex(Mlv1.HandshakeFrame())}");
        Console.WriteLine("CS_MLV1_CLIENT_FRAME bytes pinned [PASS]");

        // Every host verdict parses.
        var ok = Mlv1.ParseHandshakeReply(Utf8("MLV1 OK " + lockDigest));
        ThrowOn(ok == null || !ok.Value.Ok || ok.Value.Host != lockDigest, "MLV1 OK verdict");
        var okSelf = Mlv1.ParseHandshakeReply(Utf8("MLV1 OK " + Mlv1.ContractDigest));
        ThrowOn(okSelf == null || !okSelf.Value.Ok || okSelf.Value.Host != Mlv1.ContractDigest, "self-digest OK verdict");
        foreach (var code in new[] { "INVALID", "VERSION", "LATE" })
        {
            var bad = Mlv1.ParseHandshakeReply(Utf8("MLV1 ERR " + code));
            ThrowOn(bad == null || bad.Value.Ok || bad.Value.Error != code, $"MLV1 ERR {code} verdict");
        }
        Console.WriteLine("CS_MLV1_CLIENT_REPLY OK and all three error verdicts exact [PASS]");

        // Malformed replies refuse closed, including invalid UTF-8 and a
        // non-bytes argument.
        var refusals = new byte[]?[]
        {
            Utf8("MLV1"),
            Utf8("MLV1 "),
            Utf8("MLV1 " + Mlv1.ContractDigest.Substring(0, 15)),
            Utf8("MLV1 " + Mlv1.ContractDigest + "0"),
            Utf8("MLV1 0123456789abcdeg"),
            Utf8("MLV1 " + Mlv1.ContractDigest.ToUpperInvariant()),
            Utf8("MLV2 " + Mlv1.ContractDigest),
            Utf8("MLV1 OK " + lockDigest + " "),
            Utf8("MLV1 OK " + lockDigest.ToUpperInvariant()),
            Utf8("MLV1 OK " + lockDigest.Substring(0, 15)),
            Utf8("MLV1 ERR NOPE"),
            Utf8("MLV1 ERR VERSION "),
            Utf8(""),
            new byte[] { 0xff, 0xfe },
            null,
        };
        foreach (var bytes in refusals)
            ThrowOn(Mlv1.ParseHandshakeReply(bytes) != null, "malformed handshake reply accepted");
        Console.WriteLine("CS_MLV1_CLIENT_REFUSE malformed replies refuse closed [PASS]");
    }
}
