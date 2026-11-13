// ys_patch.h - Yamaha 4-operator voice data (YS200 / YS100 / TQ5 / B200 family, also DX21/27/100, TX81Z)
//
// SPDX-License-Identifier: MIT
//
// One voice is kept in two forms:
//   * VMEM: the 128-byte packed "memory" format used in 32-voice bulk dumps (and in our bank/state).
//   * Patch: every parameter unpacked into its own field, used by the synth and by sysex edits.
//
// Byte layouts follow the YS200 Operating Manual, "MIDI Data Format", attached lists 1-3:
//   VMEM  bytes 0-39  4 x 10 bytes operator data, stored in the order OP4, OP2, OP3, OP1
//         bytes 40-72 common data (algorithm, LFO, controllers, name, DX21 pitch EG)
//         bytes 73-83 ACED (TX81Z-style: fixed freq, fine, waveform, EG shift; reverb; FC pitch/amp)
//         bytes 84-87 ACED2 (aftertouch), 91-93 EFEDS (YS effect preset, time, balance)
//   VCED  93 single-byte parameters (voice edit buffer / parameter change group $12)
//   ACED  23 single-byte parameters (parameter change group $13, 0-22), ACED2 = $13 23-32
//   EFEDS 3 single-byte parameters (parameter change group $24, params 4-6)

#pragma once
#include <cstdint>
#include <cstring>

namespace ys {

struct OpParams
{
    uint8_t ar = 31, d1r = 0, d2r = 0, rr = 15, d1l = 15;  // envelope
    uint8_t ls = 0, rs = 0;                                // level / rate scaling
    uint8_t ebs = 0, ame = 0, kvs = 0;                     // EG bias sens, AM enable, velocity sens
    uint8_t out = 0;                                       // output level 0-99
    uint8_t crs = 4, det = 3;                              // coarse frequency (ratio index), detune 0-6
    uint8_t fix = 0, fixrg = 0, fine = 0, osw = 0, egsft = 0;   // ACED
};

struct Patch
{
    OpParams op[4];          // op[0] = OP1 ... op[3] = OP4 (OP4 has the feedback input)
    uint8_t alg = 0, fbl = 0, sync = 0;
    uint8_t lfs = 35, lfd = 0, pmd = 0, amd = 0, lfw = 2, pms = 0, ams = 0;
    uint8_t trps = 24, pbr = 2;
    uint8_t chorus = 0, mono = 0, susSw = 1, portSw = 1, portMode = 0, port = 0;
    uint8_t fcVol = 0, mwPitch = 50, mwAmp = 0, bcPitch = 0, bcAmp = 0, bcPBias = 50, bcEBias = 0;
    char    name[10] = {'I','N','I','T',' ','V','O','I','C','E'};
    uint8_t peg[6] = {99, 99, 99, 50, 50, 50};
    uint8_t rev = 0, fcPitch = 0, fcAmp = 0;
    uint8_t atPitch = 0, atAmp = 0, atPBias = 50, atEBias = 0;
    uint8_t fxPreset = 0, fxTime = 20, fxBalance = 0;
};

// Operator storage order in every Yamaha 4-op format: OP4, OP2, OP3, OP1.
static const int kMemOrder[4] = {3, 1, 2, 0};   // memory slot m -> op index

inline uint8_t lim(int v, int lo, int hi) { return (uint8_t)(v < lo ? lo : (v > hi ? hi : v)); }

inline void unpackVmem(const uint8_t* v, Patch& p)
{
    for (int m = 0; m < 4; ++m)
    {
        OpParams& o = p.op[kMemOrder[m]];
        const uint8_t* b = v + m * 10;
        o.ar = lim(b[0] & 31, 0, 31);   o.d1r = lim(b[1] & 31, 0, 31);  o.d2r = lim(b[2] & 31, 0, 31);
        o.rr = lim(b[3] & 15, 1, 15);   o.d1l = b[4] & 15;              o.ls = lim(b[5], 0, 99);
        o.ame = (b[6] >> 6) & 1;        o.ebs = (b[6] >> 3) & 7;        o.kvs = b[6] & 7;
        o.out = lim(b[7], 0, 99);       o.crs = b[8] & 63;
        o.rs = (b[9] >> 3) & 3;         o.det = lim(b[9] & 7, 0, 6);
        const uint8_t a0 = v[73 + m * 2], a1 = v[74 + m * 2];
        o.egsft = (a0 >> 4) & 3;  o.fix = (a0 >> 3) & 1;  o.fixrg = a0 & 7;
        o.osw = (a1 >> 4) & 7;    o.fine = a1 & 15;
    }
    p.sync = (v[40] >> 6) & 1;  p.fbl = (v[40] >> 3) & 7;  p.alg = v[40] & 7;
    p.lfs = lim(v[41], 0, 99);  p.lfd = lim(v[42], 0, 99);  p.pmd = lim(v[43], 0, 99);  p.amd = lim(v[44], 0, 99);
    p.pms = (v[45] >> 4) & 7;   p.ams = (v[45] >> 2) & 3;   p.lfw = v[45] & 3;
    p.trps = lim(v[46], 0, 48); p.pbr = lim(v[47], 0, 12);
    p.chorus = (v[48] >> 4) & 1; p.mono = (v[48] >> 3) & 1; p.susSw = (v[48] >> 2) & 1;
    p.portSw = (v[48] >> 1) & 1; p.portMode = v[48] & 1;
    p.port = lim(v[49], 0, 99);  p.fcVol = lim(v[50], 0, 99);
    p.mwPitch = lim(v[51], 0, 99); p.mwAmp = lim(v[52], 0, 99);
    p.bcPitch = lim(v[53], 0, 99); p.bcAmp = lim(v[54], 0, 99);
    p.bcPBias = lim(v[55], 0, 100); p.bcEBias = lim(v[56], 0, 99);
    for (int i = 0; i < 10; ++i) { char c = (char)v[57 + i]; p.name[i] = (c >= 32 && c < 127) ? c : ' '; }
    for (int i = 0; i < 6; ++i) p.peg[i] = lim(v[67 + i], 0, 99);
    p.rev = v[81] & 7;  p.fcPitch = lim(v[82], 0, 99);  p.fcAmp = lim(v[83], 0, 99);
    p.atPitch = lim(v[84], 0, 99); p.atAmp = lim(v[85], 0, 99); p.atPBias = lim(v[86], 0, 100); p.atEBias = lim(v[87], 0, 99);
    p.fxPreset = lim(v[91], 0, 10); p.fxTime = lim(v[92], 0, 40); p.fxBalance = lim(v[93], 0, 99);
}

inline void packVmem(const Patch& p, uint8_t* v)
{
    std::memset(v, 0, 128);
    for (int m = 0; m < 4; ++m)
    {
        const OpParams& o = p.op[kMemOrder[m]];
        uint8_t* b = v + m * 10;
        b[0] = o.ar; b[1] = o.d1r; b[2] = o.d2r; b[3] = o.rr; b[4] = o.d1l; b[5] = o.ls;
        b[6] = (uint8_t)((o.ame << 6) | (o.ebs << 3) | o.kvs);
        b[7] = o.out; b[8] = o.crs; b[9] = (uint8_t)((o.rs << 3) | o.det);
        v[73 + m * 2] = (uint8_t)((o.egsft << 4) | (o.fix << 3) | o.fixrg);
        v[74 + m * 2] = (uint8_t)((o.osw << 4) | o.fine);
    }
    v[40] = (uint8_t)((p.sync << 6) | (p.fbl << 3) | p.alg);
    v[41] = p.lfs; v[42] = p.lfd; v[43] = p.pmd; v[44] = p.amd;
    v[45] = (uint8_t)((p.pms << 4) | (p.ams << 2) | p.lfw);
    v[46] = p.trps; v[47] = p.pbr;
    v[48] = (uint8_t)((p.chorus << 4) | (p.mono << 3) | (p.susSw << 2) | (p.portSw << 1) | p.portMode);
    v[49] = p.port; v[50] = p.fcVol; v[51] = p.mwPitch; v[52] = p.mwAmp;
    v[53] = p.bcPitch; v[54] = p.bcAmp; v[55] = p.bcPBias; v[56] = p.bcEBias;
    for (int i = 0; i < 10; ++i) v[57 + i] = (uint8_t)p.name[i];
    for (int i = 0; i < 6; ++i) v[67 + i] = p.peg[i];
    v[81] = p.rev; v[82] = p.fcPitch; v[83] = p.fcAmp;
    v[84] = p.atPitch; v[85] = p.atAmp; v[86] = p.atPBias; v[87] = p.atEBias;
    v[91] = p.fxPreset; v[92] = p.fxTime; v[93] = p.fxBalance;
}

// ---- VCED (93 bytes / parameter change group $12) --------------------------------------------
// Returns false for an unknown parameter number.
inline bool setVced(Patch& p, int n, int x)
{
    if (n < 0) return false;
    if (n < 52)
    {
        OpParams& o = p.op[kMemOrder[n / 13]];
        switch (n % 13)
        {
            case 0: o.ar = lim(x, 0, 31); break;   case 1: o.d1r = lim(x, 0, 31); break;
            case 2: o.d2r = lim(x, 0, 31); break;  case 3: o.rr = lim(x, 1, 15); break;
            case 4: o.d1l = lim(x, 0, 15); break;  case 5: o.ls = lim(x, 0, 99); break;
            case 6: o.rs = lim(x, 0, 3); break;    case 7: o.ebs = lim(x, 0, 7); break;
            case 8: o.ame = lim(x, 0, 1); break;   case 9: o.kvs = lim(x, 0, 7); break;
            case 10: o.out = lim(x, 0, 99); break; case 11: o.crs = lim(x, 0, 63); break;
            case 12: o.det = lim(x, 0, 6); break;
        }
        return true;
    }
    switch (n)
    {
        case 52: p.alg = lim(x, 0, 7); return true;      case 53: p.fbl = lim(x, 0, 7); return true;
        case 54: p.lfs = lim(x, 0, 99); return true;     case 55: p.lfd = lim(x, 0, 99); return true;
        case 56: p.pmd = lim(x, 0, 99); return true;     case 57: p.amd = lim(x, 0, 99); return true;
        case 58: p.sync = lim(x, 0, 1); return true;     case 59: p.lfw = lim(x, 0, 3); return true;
        case 60: p.pms = lim(x, 0, 7); return true;      case 61: p.ams = lim(x, 0, 3); return true;
        case 62: p.trps = lim(x, 0, 48); return true;    case 63: p.mono = lim(x, 0, 1); return true;
        case 64: p.pbr = lim(x, 0, 12); return true;     case 65: p.portMode = lim(x, 0, 1); return true;
        case 66: p.port = lim(x, 0, 99); return true;    case 67: p.fcVol = lim(x, 0, 99); return true;
        case 68: p.susSw = lim(x, 0, 1); return true;    case 69: p.portSw = lim(x, 0, 1); return true;
        case 70: p.chorus = lim(x, 0, 1); return true;   case 71: p.mwPitch = lim(x, 0, 99); return true;
        case 72: p.mwAmp = lim(x, 0, 99); return true;   case 73: p.bcPitch = lim(x, 0, 99); return true;
        case 74: p.bcAmp = lim(x, 0, 99); return true;   case 75: p.bcPBias = lim(x, 0, 100); return true;
        case 76: p.bcEBias = lim(x, 0, 99); return true;
        default: break;
    }
    if (n >= 77 && n <= 86) { p.name[n - 77] = (x >= 32 && x < 127) ? (char)x : ' '; return true; }
    if (n >= 87 && n <= 92) { p.peg[n - 87] = lim(x, 0, 99); return true; }
    return false;
}

// ---- ACED (0-22) and ACED2 (23-32): parameter change group $13 -------------------------------
inline bool setAced(Patch& p, int n, int x)
{
    if (n >= 0 && n < 20)
    {
        OpParams& o = p.op[kMemOrder[n / 5]];
        switch (n % 5)
        {
            case 0: o.fix = lim(x, 0, 1); break;   case 1: o.fixrg = lim(x, 0, 7); break;
            case 2: o.fine = lim(x, 0, 15); break; case 3: o.osw = lim(x, 0, 7); break;
            case 4: o.egsft = lim(x, 0, 3); break;
        }
        return true;
    }
    switch (n)
    {
        case 20: p.rev = lim(x, 0, 7); return true;       case 21: p.fcPitch = lim(x, 0, 99); return true;
        case 22: p.fcAmp = lim(x, 0, 99); return true;    case 23: p.atPitch = lim(x, 0, 99); return true;
        case 24: p.atAmp = lim(x, 0, 99); return true;    case 25: p.atPBias = lim(x, 0, 100); return true;
        case 26: p.atEBias = lim(x, 0, 99); return true;
        default: return n >= 27 && n <= 32;               // reserved
    }
}

// ---- EFEDS: parameter change group $24, parameters 4-6 ------------------------------------------
inline bool setEfeds(Patch& p, int n, int x)
{
    switch (n)
    {
        case 0: p.fxPreset = lim(x, 0, 10); return true;
        case 1: p.fxTime = lim(x, 0, 40); return true;
        case 2: p.fxBalance = lim(x, 0, 99); return true;
        default: return false;
    }
}

// Algorithm carriers (bit n = op n+1 is a carrier). Yamaha 4-op algorithms 1-8.
static const uint8_t kCarriers[8] = { 0x1, 0x1, 0x1, 0x1, 0x5, 0x7, 0x7, 0xF };
inline bool isCarrier(int alg, int op) { return (kCarriers[alg & 7] >> op) & 1; }

} // namespace ys
