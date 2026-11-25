// paramtext - imprime (JSON) el texto que muestra el plugin en cada paso de un parámetro entero
// o de lista, para que los pasos de la rueda del skin digan exactamente lo mismo que el plugin.
// Uso: paramtext <plugin.so> "<nombre del parámetro>" <número de pasos>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../_tests/vsthost.h"

static std::string esc(const std::string& s)
{
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o;
}

int main(int argc, char** argv)
{
    if (argc != 4) { std::fprintf(stderr, "uso: paramtext <plugin.so> <param> <pasos>\n"); return 2; }
    th::Plug p(argv[1]);
    if (!p.ok()) return 1;
    p.prepare();
    int idx = p.find(argv[2]);
    int n = std::atoi(argv[3]);
    if (idx < 0 || n < 2) { std::fprintf(stderr, "paramtext: '%s' no existe o pasos < 2\n", argv[2]); return 1; }
    std::printf("[");
    for (int k = 0; k < n; ++k) {
        p.set(idx, (float)k / (float)(n - 1));
        std::printf("%s\"%s\"", k ? ", " : "", esc(p.text(idx)).c_str());
    }
    std::printf("]\n");
    return 0;
}
