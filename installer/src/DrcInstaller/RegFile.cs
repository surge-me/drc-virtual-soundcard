using System.Globalization;
using System.Text;
using Microsoft.Win32;

namespace DrcInstaller;

internal sealed record RegValue(RegistryValueKind Kind, object? Data);

/// <summary>
/// 极简 .reg 读写（仅覆盖 FxProperties 场景所需的类型）。
/// 写出格式：Windows Registry Editor Version 5.00 + 单个键块。
/// 只解析本工具自己写出的格式（REG_SZ/DWORD/QWORD/MULTI_SZ/BINARY/EXPAND_SZ）。
/// </summary>
internal static class RegFile
{
    private const string Header = "Windows Registry Editor Version 5.00";

    // ---------------- 写 ----------------

    public static void Write(string path, string hklmKeyPath, IReadOnlyDictionary<string, RegValue> values)
    {
        var sb = new StringBuilder();
        sb.Append(Header).Append("\r\n\r\n");
        sb.Append('[').Append(hklmKeyPath).Append("]\r\n");
        foreach (var kv in values)
        {
            sb.Append('"').Append(Escape(kv.Key)).Append("\"=").Append(FormatValue(kv.Value)).Append("\r\n");
        }
        File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
    }

    private static string Escape(string s) => s.Replace("\\", "\\\\").Replace("\"", "\\\"");

    private static string FormatValue(RegValue v)
    {
        switch (v.Kind)
        {
            case RegistryValueKind.String:
                return "\"" + Escape(v.Data as string ?? string.Empty) + "\"";
            case RegistryValueKind.ExpandString:
                return "hex(2):" + HexBytes(Encoding.Unicode.GetBytes((v.Data as string ?? string.Empty) + "\0"));
            case RegistryValueKind.DWord:
                return "dword:" + ((uint)Convert.ToInt32(v.Data, CultureInfo.InvariantCulture)).ToString("x8", CultureInfo.InvariantCulture);
            case RegistryValueKind.QWord:
                return "hex(b):" + HexBytes(BitConverter.GetBytes((ulong)Convert.ToInt64(v.Data, CultureInfo.InvariantCulture)));
            case RegistryValueKind.MultiString:
                return "hex(7):" + HexBytes(MultiStringBytes((string[])(v.Data ?? Array.Empty<string>())));
            case RegistryValueKind.Binary:
                return "hex:" + HexBytes((byte[])(v.Data ?? Array.Empty<byte>()));
            default:
                return "hex:" + HexBytes(Array.Empty<byte>());
        }
    }

    private static byte[] MultiStringBytes(string[] parts)
    {
        var list = new List<byte>();
        foreach (var p in parts) list.AddRange(Encoding.Unicode.GetBytes(p + "\0"));
        list.AddRange(new byte[] { 0, 0 }); // 终止空串
        return list.ToArray();
    }

    private static string HexBytes(byte[] bytes) =>
        bytes.Length == 0 ? "" : string.Join(",", Array.ConvertAll(bytes, b => b.ToString("x2", CultureInfo.InvariantCulture)));

    // ---------------- 读 ----------------

    public static Dictionary<string, RegValue> Read(string path)
    {
        var result = new Dictionary<string, RegValue>(StringComparer.OrdinalIgnoreCase);
        foreach (var raw in File.ReadAllLines(path))
        {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith(';') || line.StartsWith('[')) continue;
            if (line.Equals(Header, StringComparison.OrdinalIgnoreCase)) continue;

            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            string namePart = line.Substring(0, eq).Trim();
            string valPart = line.Substring(eq + 1).Trim();

            string name = namePart == "@" ? string.Empty : Unescape(TrimQuotes(namePart));
            result[name] = ParseValue(valPart);
        }
        return result;
    }

    private static RegValue ParseValue(string v)
    {
        if (v.StartsWith('"')) return new RegValue(RegistryValueKind.String, Unescape(TrimQuotes(v)));
        if (v.StartsWith("dword:", StringComparison.OrdinalIgnoreCase))
            return new RegValue(RegistryValueKind.DWord, (int)uint.Parse(v.Substring(6), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
        if (v.StartsWith("hex(b):", StringComparison.OrdinalIgnoreCase))
            return new RegValue(RegistryValueKind.QWord, (long)BitConverter.ToUInt64(ParseHex(v.Substring(7)), 0));
        if (v.StartsWith("hex(7):", StringComparison.OrdinalIgnoreCase))
            return new RegValue(RegistryValueKind.MultiString, ParseMultiString(ParseHex(v.Substring(7))));
        if (v.StartsWith("hex(2):", StringComparison.OrdinalIgnoreCase))
            return new RegValue(RegistryValueKind.ExpandString, Encoding.Unicode.GetString(ParseHex(v.Substring(7))).TrimEnd('\0'));
        if (v.StartsWith("hex:", StringComparison.OrdinalIgnoreCase))
            return new RegValue(RegistryValueKind.Binary, ParseHex(v.Substring(4)));
        // 未知形式 → 按字符串兜底
        return new RegValue(RegistryValueKind.String, v);
    }

    private static byte[] ParseHex(string s)
    {
        s = s.Replace("\\\r\n", "").Replace("\\\n", "").Replace(" ", "");
        if (s.Length == 0) return Array.Empty<byte>();
        var parts = s.Split(',', StringSplitOptions.RemoveEmptyEntries);
        var bytes = new byte[parts.Length];
        for (int i = 0; i < parts.Length; i++) bytes[i] = Convert.ToByte(parts[i], 16);
        return bytes;
    }

    private static string[] ParseMultiString(byte[] bytes)
    {
        string all = Encoding.Unicode.GetString(bytes);
        return all.Split('\0').Where(s => s.Length > 0).ToArray();
    }

    private static string TrimQuotes(string s) =>
        s.Length >= 2 && s[0] == '"' && s[^1] == '"' ? s.Substring(1, s.Length - 2) : s;

    private static string Unescape(string s) => s.Replace("\\\"", "\"").Replace("\\\\", "\\");
}
