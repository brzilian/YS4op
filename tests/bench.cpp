#include "harness_inc.h"
int main(int, char** argv)
{
    void* h = dlopen(argv[1], RTLD_NOW); entry = (Entry)dlsym(h, "VSTPluginMain");
    for (int prog : {0, 7, 45, 79})
      for (int fxOff = 0; fxOff < 2; ++fxOff)
        for (int nv : {1, 16})
        {
            Inst p; p.disp(effSetProgram, 0, prog); if (fxOff) p.param(17, 0.0f);
            p.render(0.05);
            for (int i = 0; i < nv; ++i) p.note(48 + i * 2, 100);
            float L[128], R[128]; double sum = 0, worst = 0;
            for (int b = 0; b < 3000; ++b) { p.block(L, R, 128); if (b > 10) { sum += p.lastUs; worst = std::fmax(worst, p.lastUs); } }
            std::printf("prog %2d fx %s voices %2d: avg %6.1f us worst %6.1f\n", prog, fxOff ? "off" : "on ", nv, sum / 2989, worst);
        }
}
