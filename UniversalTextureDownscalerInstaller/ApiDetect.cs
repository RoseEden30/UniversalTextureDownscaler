using System.Runtime.InteropServices;

namespace UniversalTextureDownscalerInstaller;

public enum GraphicsApi { Unknown, D3D11, D3D12, Vulkan }

/// Reads an exe's PE import table to see which of d3d11.dll/d3d12.dll/vulkan-1.dll
/// it imports directly.
public static class ApiDetect
{
    // Unreal Engine's bootstrap exe embeds the real game exe's relative path as
    // RCDATA resource #201 (BootstrapPackagedGame.cpp). No-op otherwise.
    public static string ResolveRealExecutable(string exePath)
    {
        var module = LoadLibraryExW(exePath, IntPtr.Zero, LOAD_LIBRARY_AS_DATAFILE);
        if (module == IntPtr.Zero) return exePath;

        try
        {
            var info = FindResourceW(module, "#201", "#10");  // IDI_EXEC_FILE, RT_RCDATA
            if (info == IntPtr.Zero) return exePath;

            var handle = LoadResource(module, info);
            var data = LockResource(handle);
            var size = SizeofResource(module, info);
            if (handle == IntPtr.Zero || data == IntPtr.Zero || size < 2) return exePath;

            var relative = Marshal.PtrToStringUni(data, (int)(size / 2) - 1);
            if (string.IsNullOrEmpty(relative)) return exePath;

            var resolved = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(exePath) ?? "", relative));
            return File.Exists(resolved) ? resolved : exePath;
        }
        finally
        {
            FreeLibrary(module);
        }
    }

    public static GraphicsApi Detect(string exePath)
    {
        try
        {
            var imports = ImportedModuleNames(exePath);
            if (imports.Contains("d3d12.dll")) return GraphicsApi.D3D12;
            if (imports.Contains("d3d11.dll")) return GraphicsApi.D3D11;
            if (imports.Contains("vulkan-1.dll")) return GraphicsApi.Vulkan;
            return GraphicsApi.Unknown;
        }
        catch
        {
            // A malformed or unreadable exe just means "couldn't tell", the user can
            // still pick manually, this is never the only way to choose an API.
            return GraphicsApi.Unknown;
        }
    }

    private static HashSet<string> ImportedModuleNames(string exePath)
    {
        using var stream = new FileStream(exePath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var reader = new BinaryReader(stream);

        // DOS header: "MZ" then e_lfanew at offset 0x3C points to the PE header.
        if (reader.ReadUInt16() != 0x5A4D) return [];
        stream.Position = 0x3C;
        var peOffset = reader.ReadUInt32();

        stream.Position = peOffset;
        if (reader.ReadUInt32() != 0x00004550) return [];  // "PE\0\0"

        // IMAGE_FILE_HEADER, 20 bytes, starting right after the signature:
        // Machine(2) NumberOfSections(2) TimeDateStamp(4) PointerToSymbolTable(4)
        // NumberOfSymbols(4) SizeOfOptionalHeader(2) Characteristics(2).
        var fileHeaderOffset = peOffset + 4;
        stream.Position = fileHeaderOffset + 2;
        var numberOfSections = reader.ReadUInt16();
        stream.Position = fileHeaderOffset + 16;
        var sizeOfOptionalHeader = reader.ReadUInt16();

        var optionalHeaderOffset = fileHeaderOffset + 20;
        var sectionTableOffset = optionalHeaderOffset + sizeOfOptionalHeader;

        stream.Position = optionalHeaderOffset;
        var magic = reader.ReadUInt16();
        var isPe32Plus = magic == 0x20B;
        if (!isPe32Plus && magic != 0x10B) return [];

        // DataDirectory[16] sits right after the optional header's fixed fields (96
        // bytes for PE32, 112 for PE32+). Entry 1 is the regular Import Directory;
        // entry 13 is the Delay Import Directory, used by engines that delay-load
        // their graphics DLLs to switch backends at runtime, checking only entry 1
        // would miss exactly the DLLs this detector cares about most.
        var dataDirectoryOffset = optionalHeaderOffset + (isPe32Plus ? 112u : 96u);

        // NumberOfRvaAndSizes is the last field before the directories and is what
        // bounds them, it is 16 in practice but the spec allows fewer, in which case
        // reading entry 1 or 13 anyway would read section-table bytes as an RVA.
        stream.Position = dataDirectoryOffset - 4;
        var numberOfRvaAndSizes = reader.ReadUInt32();

        var sections = new List<(uint VirtualAddress, uint Size, uint PointerToRawData)>();
        stream.Position = sectionTableOffset;
        for (var i = 0; i < numberOfSections; i++)
        {
            stream.Position += 8;  // Name
            var virtualSize = reader.ReadUInt32();
            var virtualAddress = reader.ReadUInt32();
            var sizeOfRawData = reader.ReadUInt32();
            var pointerToRawData = reader.ReadUInt32();
            stream.Position += 16;  // remaining fields
            // VirtualSize can legitimately be 0 (some linkers leave it unset and let
            // SizeOfRawData describe the section), which would make every RVA in that
            // section unresolvable; the larger of the two is what actually covers it.
            sections.Add((virtualAddress, Math.Max(virtualSize, sizeOfRawData), pointerToRawData));
        }

        long? RvaToOffset(uint rva)
        {
            foreach (var (va, size, ptr) in sections)
                if (rva >= va && rva < va + size)
                {
                    var offset = ptr + (long)(rva - va);
                    return offset < stream.Length ? offset : null;
                }
            return null;
        }

        string? ReadAsciiStringAt(long offset)
        {
            stream.Position = offset;
            var bytes = new List<byte>();
            int b;
            // Bounded: a bad RVA into a stretch with no NUL byte would otherwise
            // buffer a large part of a multi-hundred-MB game exe. No real DLL name
            // comes close to this.
            while (bytes.Count < 512 && (b = stream.ReadByte()) > 0) bytes.Add((byte)b);
            return System.Text.Encoding.ASCII.GetString(bytes.ToArray());
        }

        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        // Regular imports: array of IMAGE_IMPORT_DESCRIPTOR (20 bytes), Name RVA at
        // offset 12, terminated by an all-zero entry.
        uint DataDirectoryRva(uint index)
        {
            if (index >= numberOfRvaAndSizes) return 0;
            stream.Position = dataDirectoryOffset + index * 8;
            return reader.ReadUInt32();
        }

        var importRva = DataDirectoryRva(1);
        if (importRva != 0 && RvaToOffset(importRva) is { } importOffset)
        {
            stream.Position = importOffset;
            while (true)
            {
                stream.Position += 12;  // OriginalFirstThunk, TimeDateStamp, ForwarderChain
                var nameRva = reader.ReadUInt32();
                stream.Position += 4;  // FirstThunk
                if (nameRva == 0) break;

                if (RvaToOffset(nameRva) is { } nameOffset)
                {
                    var next = stream.Position;
                    if (ReadAsciiStringAt(nameOffset) is { } name) names.Add(name);
                    stream.Position = next;
                }
            }
        }

        // Delay-load imports: array of IMAGE_DELAYLOAD_DESCRIPTOR (32 bytes), DllNameRVA
        // at offset 4, likewise terminated by an all-zero entry.
        var delayRva = DataDirectoryRva(13);
        if (delayRva != 0 && RvaToOffset(delayRva) is { } delayOffset)
        {
            stream.Position = delayOffset;
            while (true)
            {
                var attributes = reader.ReadUInt32();
                var nameRva = reader.ReadUInt32();
                stream.Position += 24;  // ModuleHandle..TimeDateStamp
                if (attributes == 0 && nameRva == 0) break;

                if (RvaToOffset(nameRva) is { } nameOffset)
                {
                    var next = stream.Position;
                    if (ReadAsciiStringAt(nameOffset) is { } name) names.Add(name);
                    stream.Position = next;
                }
            }
        }

        return names;
    }

    private const uint LOAD_LIBRARY_AS_DATAFILE = 0x2;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibraryExW(string lpFileName, IntPtr hFile, uint dwFlags);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr FindResourceW(IntPtr hModule, string lpName, string lpType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LoadResource(IntPtr hModule, IntPtr hResInfo);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr LockResource(IntPtr hResData);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint SizeofResource(IntPtr hModule, IntPtr hResInfo);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool FreeLibrary(IntPtr hLibModule);
}
