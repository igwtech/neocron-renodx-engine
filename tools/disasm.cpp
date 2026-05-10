#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *PFN_Disasm)(
    LPCVOID pSrcData, SIZE_T SrcDataSize, UINT Flags,
    LPCSTR szComments, ID3DBlob** ppDisassembly);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <shader.dxbc>\n", argv[0]); return 1; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);

    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    if (!m) { fprintf(stderr, "LoadLibrary failed\n"); return 1; }
    PFN_Disasm fn = (PFN_Disasm)GetProcAddress(m, "D3DDisassemble");
    if (!fn) { fprintf(stderr, "GetProcAddress failed\n"); return 1; }

    ID3DBlob* asm_blob = NULL;
    HRESULT hr = fn(data, sz, 0, NULL, &asm_blob);
    if (FAILED(hr)) { fprintf(stderr, "D3DDisassemble: 0x%08x\n", (unsigned)hr); return 1; }
    fwrite(asm_blob->GetBufferPointer(), 1, asm_blob->GetBufferSize() - 1, stdout);
    return 0;
}
