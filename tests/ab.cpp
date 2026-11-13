// A/B demo: each program played with the old float engine, then the new OPZ engine.
#include "harness_inc.h"
static std::vector<float> L, R;
static void phrase(Entry en, int prog) {
    entry = en; Inst p; p.disp(effSetProgram, 0, prog); p.render(0.05);
    const int notes[4] = {60, 64, 67, 72};
    for (int k = 0; k < 4; ++k) { p.note(notes[k] - 12 * (prog >= 60 && prog <= 64), 100); std::vector<float> r; auto l = p.render(0.25, &r); L.insert(L.end(), l.begin(), l.end()); R.insert(R.end(), r.begin(), r.end()); }
    std::vector<float> r; auto l = p.render(1.0, &r); L.insert(L.end(), l.begin(), l.end()); R.insert(R.end(), r.begin(), r.end());
    for (int k = 0; k < 4; ++k) p.note(notes[k] - 12 * (prog >= 60 && prog <= 64), 0);
    r.clear(); l = p.render(0.9, &r); L.insert(L.end(), l.begin(), l.end()); R.insert(R.end(), r.begin(), r.end());
    L.insert(L.end(), 8820, 0.0f); R.insert(R.end(), 8820, 0.0f);
}
int main(int, char** argv) {
    Entry oldE = (Entry)dlsym(dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), "VSTPluginMain");
    Entry newE = (Entry)dlsym(dlopen(argv[2], RTLD_NOW | RTLD_LOCAL), "VSTPluginMain");
    for (int prog : {0, 7, 8, 10, 45, 62, 71, 79, 85, 97}) { phrase(oldE, prog); phrase(newE, prog); L.insert(L.end(), 22050, 0.0f); R.insert(R.end(), 22050, 0.0f); }
    wav(argv[3], L, R); std::printf("%.1f s\n", L.size() / 44100.0);
}
