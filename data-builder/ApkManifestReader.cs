using System.Text;
using System.Xml.Linq;

namespace McsmVitaDataBuilder;

public sealed record ApkManifestInfo(string PackageName, int VersionCode, string VersionName);

public static class ApkManifestReader
{
    private const ushort StringPoolType = 0x0001;
    private const ushort StartElementType = 0x0102;
    private const uint NoIndex = 0xFFFFFFFF;
    private const uint Utf8Flag = 0x00000100;
    private const byte TypeString = 0x03;
    private const byte TypeIntDecimal = 0x10;
    private const byte TypeIntHex = 0x11;

    public static ApkManifestInfo Read(Stream manifestStream)
    {
        using MemoryStream memory = new();
        manifestStream.CopyTo(memory);
        byte[] data = memory.ToArray();
        if (data.Length == 0)
        {
            throw new InvalidDataException("AndroidManifest.xml is empty.");
        }

        int firstContent = Array.FindIndex(data, value => !char.IsWhiteSpace((char)value));
        return firstContent >= 0 && data[firstContent] == (byte)'<'
            ? ReadPlainXml(data)
            : ReadBinaryXml(data);
    }

    private static ApkManifestInfo ReadPlainXml(byte[] data)
    {
        XDocument document = XDocument.Parse(Encoding.UTF8.GetString(data));
        XElement manifest = document.Root
            ?? throw new InvalidDataException("AndroidManifest.xml has no manifest element.");
        string packageName = manifest.Attribute("package")?.Value ?? string.Empty;
        string versionCodeText = manifest.Attributes()
            .FirstOrDefault(attribute => attribute.Name.LocalName == "versionCode")?.Value ?? string.Empty;
        string versionName = manifest.Attributes()
            .FirstOrDefault(attribute => attribute.Name.LocalName == "versionName")?.Value ?? string.Empty;
        if (!int.TryParse(versionCodeText, out int versionCode))
        {
            throw new InvalidDataException("AndroidManifest.xml does not contain a numeric versionCode.");
        }
        return Validate(packageName, versionCode, versionName);
    }

    private static ApkManifestInfo ReadBinaryXml(byte[] data)
    {
        if (data.Length < 8 || ReadUInt16(data, 0) != 0x0003)
        {
            throw new InvalidDataException("AndroidManifest.xml is not valid Android binary XML.");
        }

        IReadOnlyList<string>? strings = null;
        int offset = ReadUInt16(data, 2);
        while (offset + 8 <= data.Length)
        {
            ushort type = ReadUInt16(data, offset);
            ushort headerSize = ReadUInt16(data, offset + 2);
            uint chunkSizeValue = ReadUInt32(data, offset + 4);
            if (headerSize < 8 || chunkSizeValue < headerSize || chunkSizeValue > int.MaxValue)
            {
                throw new InvalidDataException("AndroidManifest.xml contains a malformed chunk.");
            }
            int chunkSize = (int)chunkSizeValue;
            if (offset > data.Length - chunkSize)
            {
                throw new InvalidDataException("AndroidManifest.xml contains a truncated chunk.");
            }

            if (type == StringPoolType)
            {
                strings = ReadStringPool(data, offset, headerSize, chunkSize);
            }
            else if (type == StartElementType && strings is not null)
            {
                ApkManifestInfo? info = TryReadManifestElement(data, offset, headerSize, chunkSize, strings);
                if (info is not null)
                {
                    return info;
                }
            }

            offset += chunkSize;
        }

        throw new InvalidDataException("AndroidManifest.xml does not contain readable package/version metadata.");
    }

    private static IReadOnlyList<string> ReadStringPool(
        byte[] data,
        int chunkOffset,
        int headerSize,
        int chunkSize)
    {
        if (headerSize < 28)
        {
            throw new InvalidDataException("AndroidManifest.xml contains an invalid string pool.");
        }

        uint stringCountValue = ReadUInt32(data, chunkOffset + 8);
        uint flags = ReadUInt32(data, chunkOffset + 16);
        uint stringsStartValue = ReadUInt32(data, chunkOffset + 20);
        if (stringCountValue > int.MaxValue || stringsStartValue > int.MaxValue)
        {
            throw new InvalidDataException("AndroidManifest.xml string pool is too large.");
        }
        int stringCount = (int)stringCountValue;
        int stringsStart = (int)stringsStartValue;
        int offsetsStart = chunkOffset + headerSize;
        if (stringCount > (chunkSize - headerSize) / 4
            || stringsStart < headerSize
            || stringsStart >= chunkSize)
        {
            throw new InvalidDataException("AndroidManifest.xml string pool offsets are invalid.");
        }

        bool utf8 = (flags & Utf8Flag) != 0;
        var result = new string[stringCount];
        for (int index = 0; index < stringCount; index++)
        {
            uint relativeValue = ReadUInt32(data, offsetsStart + index * 4);
            if (relativeValue > int.MaxValue)
            {
                throw new InvalidDataException("AndroidManifest.xml contains an invalid string offset.");
            }
            int stringOffset = chunkOffset + stringsStart + (int)relativeValue;
            int chunkEnd = chunkOffset + chunkSize;
            result[index] = utf8
                ? ReadUtf8String(data, stringOffset, chunkEnd)
                : ReadUtf16String(data, stringOffset, chunkEnd);
        }
        return result;
    }

    private static ApkManifestInfo? TryReadManifestElement(
        byte[] data,
        int chunkOffset,
        int headerSize,
        int chunkSize,
        IReadOnlyList<string> strings)
    {
        int extension = chunkOffset + headerSize;
        if (extension + 20 > chunkOffset + chunkSize)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a malformed element.");
        }

        string? elementName = GetString(strings, ReadUInt32(data, extension + 4));
        if (!string.Equals(elementName, "manifest", StringComparison.Ordinal))
        {
            return null;
        }

        int attributeStart = ReadUInt16(data, extension + 8);
        int attributeSize = ReadUInt16(data, extension + 10);
        int attributeCount = ReadUInt16(data, extension + 12);
        if (attributeSize < 20 || attributeCount > (chunkSize - headerSize - attributeStart) / attributeSize)
        {
            throw new InvalidDataException("AndroidManifest.xml contains malformed attributes.");
        }

        string packageName = string.Empty;
        string versionName = string.Empty;
        int? versionCode = null;
        int attributeOffset = extension + attributeStart;
        for (int index = 0; index < attributeCount; index++)
        {
            int current = attributeOffset + index * attributeSize;
            string? name = GetString(strings, ReadUInt32(data, current + 4));
            uint rawValue = ReadUInt32(data, current + 8);
            byte valueType = data[current + 15];
            uint valueData = ReadUInt32(data, current + 16);
            string? textValue = rawValue != NoIndex
                ? GetString(strings, rawValue)
                : valueType == TypeString
                    ? GetString(strings, valueData)
                    : null;

            if (string.Equals(name, "package", StringComparison.Ordinal))
            {
                packageName = textValue ?? string.Empty;
            }
            else if (string.Equals(name, "versionName", StringComparison.Ordinal))
            {
                versionName = textValue ?? string.Empty;
            }
            else if (string.Equals(name, "versionCode", StringComparison.Ordinal))
            {
                if (valueType is TypeIntDecimal or TypeIntHex)
                {
                    versionCode = unchecked((int)valueData);
                }
                else if (int.TryParse(textValue, out int parsed))
                {
                    versionCode = parsed;
                }
            }
        }

        if (versionCode is null)
        {
            throw new InvalidDataException("AndroidManifest.xml does not contain a numeric versionCode.");
        }
        return Validate(packageName, versionCode.Value, versionName);
    }

    private static ApkManifestInfo Validate(string packageName, int versionCode, string versionName)
    {
        if (string.IsNullOrWhiteSpace(packageName))
        {
            throw new InvalidDataException("AndroidManifest.xml does not contain a package name.");
        }
        return new ApkManifestInfo(packageName, versionCode, versionName);
    }

    private static string ReadUtf8String(byte[] data, int offset, int end)
    {
        _ = ReadLength8(data, ref offset, end);
        int byteLength = ReadLength8(data, ref offset, end);
        if (byteLength < 0 || offset > end - byteLength)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a truncated UTF-8 string.");
        }
        return Encoding.UTF8.GetString(data, offset, byteLength);
    }

    private static int ReadLength8(byte[] data, ref int offset, int end)
    {
        if (offset >= end)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a truncated UTF-8 length.");
        }
        int length = data[offset++];
        if ((length & 0x80) == 0)
        {
            return length;
        }
        if (offset >= end)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a truncated UTF-8 length.");
        }
        return ((length & 0x7F) << 8) | data[offset++];
    }

    private static string ReadUtf16String(byte[] data, int offset, int end)
    {
        int charLength = ReadLength16(data, ref offset, end);
        int byteLength = checked(charLength * 2);
        if (offset > end - byteLength)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a truncated UTF-16 string.");
        }
        return Encoding.Unicode.GetString(data, offset, byteLength);
    }

    private static int ReadLength16(byte[] data, ref int offset, int end)
    {
        if (offset > end - 2)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a truncated UTF-16 length.");
        }
        int length = ReadUInt16(data, offset);
        offset += 2;
        if ((length & 0x8000) == 0)
        {
            return length;
        }
        if (offset > end - 2)
        {
            throw new InvalidDataException("AndroidManifest.xml contains a truncated UTF-16 length.");
        }
        int low = ReadUInt16(data, offset);
        offset += 2;
        return ((length & 0x7FFF) << 16) | low;
    }

    private static string? GetString(IReadOnlyList<string> strings, uint index) =>
        index == NoIndex || index >= (uint)strings.Count ? null : strings[(int)index];

    private static ushort ReadUInt16(byte[] data, int offset)
    {
        if (offset < 0 || offset > data.Length - 2)
        {
            throw new InvalidDataException("AndroidManifest.xml ended unexpectedly.");
        }
        return (ushort)(data[offset] | (data[offset + 1] << 8));
    }

    private static uint ReadUInt32(byte[] data, int offset)
    {
        if (offset < 0 || offset > data.Length - 4)
        {
            throw new InvalidDataException("AndroidManifest.xml ended unexpectedly.");
        }
        return (uint)(data[offset]
            | (data[offset + 1] << 8)
            | (data[offset + 2] << 16)
            | (data[offset + 3] << 24));
    }
}
