#!/usr/bin/env python3
"""cvframes_sim.py -- core 1's bus loop of a cvframes plugin, run against the
Game Boy kernel's own bus cycles.

    python3 -I cvframes_sim.py PLUGIN.elf [--kernel gbkernel.bin] [--lists lists.bin]
            [--frames N] [--bounce [--hit P]] [--publish-us N] [--seed N] [--quiet]

The console's side is the kernel itself (gbcv/kernel/gbkernel.asm, the image
the plugin's build made: PLUGIN.elf's folder has cvframes/gbkernel.bin),
interpreted here instruction by instruction and laid out as Z80 machine
cycles (cvplay/CVSERVE.md's timing; sdplayer/test/cvserve_sim.py's bus):
its opcode fetches and operands from the fixed bank, its list's bytes from
$C000 on, the four selects of a frame's start, the NMI through the BIOS's
vector, the I/O cycles of its VDP and PSG writes (with the PSG's wait), the
stack and its counters in RAM. The lists are sw_host's (--lists, as
starwars/test/console.sh makes them), or made up here in their shapes: runs
at the vertical blank's pace and the display's, PSG bytes, the speech lists'
hundred samples.

The cart's side is the plugin's object code, run by cvserve_sim.py's
Thumb-2 interpreter with its cycle counts: cvframes.c's serve, on_select and
on_header, or cvfserve.S's cvf_serve2 (cvframes_serve2() in the plugin's
CMakeLists.txt), whichever the ELF has. "Core 0" publishes each list
--publish-us after the console took the one before (300: while that one is
still running, as when core 0 is a list ahead).

Checked, for every cart read: the right byte on the cart's pins from set-up
time before the Z80 samples it; and that every select was taken, once, each
frame's list with it, the controller's nibbles as reported, and nothing
else. The smallest margins are printed.

--bounce: what the console's captures show as well (CVSERVE.md): the address
lines and nCART may read wrong at the cart's pins, either way, for 30 ns from
a change of the cart's byte while nCART is low, and for 30 ns from nCART's
fall (the 74HCT245 switching the console's bus). --hit scales how often
(1.0: cvserve_sim.py's own odds, more than the console's; 0.05 is nearer).
"""
import argparse, bisect, os, random, re as _re, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'sdplayer', 'test'))
import cvserve_sim as cs


class _Re:                                 # cvserve_sim.py parses each instruction with re as it runs it: once will do
    def __init__(self): self.c = {}
    def split(self, pat, s):
        k = (0, pat, s)
        if k not in self.c: self.c[k] = _re.split(pat, s)
        return self.c[k]
    def match(self, pat, s):
        k = (1, pat, s)
        if k not in self.c: self.c[k] = _re.match(pat, s)
        return self.c[k]
    def search(self, pat, s):
        k = (2, pat, s)
        if k not in self.c: self.c[k] = _re.search(pat, s)
        return self.c[k]
cs.re = _Re()

T, CYCLE, NCART = cs.T, cs.CYCLE, cs.NCART
FRAME_T = 262 * 228
BANK = 0x4000


def mega_index(a):                         # perm.h's MEGA_INDEX of a bank offset's pins
    r = cs.raw(a & 0x3fff)
    return (r & 0xff) | (r >> 1 & 0x3f00)


# ---- lists in swcv's shapes, when sw_host's aren't given ------------------------
def made_up_lists(n, rng):
    out = []
    for k in range(n):
        l = bytearray()
        def run(nb):
            a = rng.choice([rng.randrange(0x1800), 0x2000 + rng.randrange(0x1800)])
            l.extend([1, a & 255, (a >> 8 & 0x3f) | 0x40, nb])
            l.extend(rng.choice([0, 0, 0, 0x80, 0x18, 0xff, 0xf1, 0x51, 0x01, 0x03, 0x02]) if rng.random() < 0.8 else rng.randrange(256) for _ in range(nb))
        if k % 4 == 3:                     # a speech list: a sample's bytes, then VRAM to the next
            l.append(0)
            for s in range(100):
                nb = rng.choice([2, 2, 2, 3, 4, 6])
                l.extend([2, nb]); l.extend(rng.choice([0xb0, 0xd0, 0x90, 0xf0]) | rng.randrange(16) for _ in range(nb))
                if s < 99:
                    if rng.random() < 0.2: run(rng.randrange(1, 3))
                    run(rng.randrange(8, 13))
            l.append(0)
        else:                              # a plain one: the blank's part, then the display's
            if rng.random() < 0.5:
                nb = rng.randrange(1, 12)
                l.extend([2, nb]); l.extend(0x80 | rng.randrange(128) for _ in range(nb))
            t = 0
            while t < 520 and rng.random() < 0.93:
                nb = rng.choice([rng.randrange(1, 12), rng.randrange(1, 12), 255, rng.randrange(129, 256)])
                nb = min(nb, 600 - t)
                run(nb); t += nb
            l.append(0)
            t = 0
            for _ in range(rng.choice([0, 0, 1, 3, 30])):
                nb = min(rng.choice([rng.randrange(1, 10), 255]), 1200 - t)
                if nb < 1: break
                run(nb); t += nb
            l.append(0)
        out.append(bytes(l))
    return out


def read_lists(path, first, n):
    d = open(path, 'rb').read()
    out, i = [], 0
    while i < len(d):
        k = d[i] | d[i + 1] << 8
        out.append(d[i + 2:i + 2 + k]); i += 2 + k
    return out[first:first + n]


# ---- the console: gbkernel, as bus cycles -------------------------------------------
class Kernel:
    def __init__(self, image, bus, lists, rng, psg_wait=29):
        self.img, self.bus, self.lists, self.rng, self.psg_wait = image, bus, lists, rng, psg_wait
        self.ram = bytearray(0x400)
        self.a = self.b = self.c = self.d = self.e = self.h = self.l = 0
        self.z = False
        self.sp, self.pc, self.r = 0x7400, 0x8065, 0          # frame: (the VDP and PSG are set up)
        self.F = False                     # the VDP's flag
        self.next_vbl = bus.t + 200 * T
        self.frame = -1                    # the list at $C000
        self.exp = []                      # the byte each of bus.checks should read (None: any)
        self.frame_sel = []                # (index into bus.selects, frame) of each frame's start
        self.reports = []                  # (index into bus.selects, slot, nibble)
        self.ended = False

    def peek(self, a):
        if a >= 0xc000:
            if a >= 0xffc0: return 0
            l = self.lists[self.frame] if 0 <= self.frame < len(self.lists) else b'\0\0'
            return l[a - 0xc000] if a - 0xc000 < len(l) else 0
        if a >= 0x8000: return self.img[a - 0x8000]
        if a >= 0x6000: return self.ram[a & 0x3ff]
        return {0x66: 0xc3, 0x67: 0x21, 0x68: 0x80}.get(a, 0xff)

    def _cyc(self, kind, a, refresh=0):
        n0, s0 = len(self.bus.checks), len(self.bus.selects)
        v = self.peek(a)
        self.bus.cycle(kind, a, refresh)
        if len(self.bus.checks) > n0: self.exp.append(None if a >= 0xffc0 or (a >= 0xc000 and self.frame >= len(self.lists)) else v)
        if len(self.bus.selects) > s0:
            n = a & 63
            if n >= 48:
                self.frame += 1
                self.frame_sel.append((s0, self.frame))
                if self.frame >= len(self.lists): self.ended = True
            else: self.reports.append((s0, n >> 4, n & 15))
        return v

    def m1(self):
        self.r = (self.r + 1) & 0x7f
        v = self._cyc('m1', self.pc, self.r)
        self.pc += 1
        return v

    def imm(self):
        v = self._cyc('rd', self.pc); self.pc += 1
        return v

    def rd(self, a): return self._cyc('rd', a)

    def wr(self, a, v):
        self.bus.cycle('wr', a)
        if 0x6000 <= a < 0x8000: self.ram[a & 0x3ff] = v

    def io_out(self, port_hi, port, v):
        self.bus.cycle('io', port_hi << 8 | port)
        if port == 0xff: self.bus.idle(self.psg_wait)       # the SN76489's READY holds the Z80

    def io_in(self, port_hi, port):
        self.bus.cycle('io', port_hi << 8 | port)
        if port == 0xbf:
            v = 0x80 if self.F else 0
            self.F = False
            return v
        return 0x7f                        # the controller: nothing pressed

    def hl(self): return self.h << 8 | self.l
    def set_hl(self, v): self.h, self.l = v >> 8 & 255, v & 255

    def nmi(self):
        self._cyc('m1', self.pc, self.r)   # the acknowledge: an opcode fetch whose byte isn't used
        self.bus.idle(1)
        self.wr(self.sp - 1 & 0xffff, self.pc >> 8); self.wr(self.sp - 2 & 0xffff, self.pc & 255)
        self.sp = self.sp - 2 & 0xffff
        self.pc = 0x66

    def step(self):
        bus = self.bus
        if bus.t >= self.next_vbl:         # the vertical blank: the flag, and the NMI if it was clear
            self.next_vbl += FRAME_T * T
            if not self.F:
                self.F = True
                self.nmi()
                return
        # waiting for the vertical blank with nothing to do: the bus is left quiet until just before it
        if self.pc == 0x806b and self.ram[0] == self.ram[1] and self.next_vbl - bus.t > 400e3 and not self.F:
            for _ in range(2):
                self.m1(); self.rd(self.hl()); self.m1(); self.imm(); bus.idle(5); self.pc = 0x806b
            bus.idle(int((self.next_vbl - bus.t - 150e3) / T))
            return
        op = self.m1()
        if op == 0xf5: bus.idle(1); self.wr(self.sp - 1 & 0xffff, self.a); self.wr(self.sp - 2 & 0xffff, 0); self.sp = self.sp - 2 & 0xffff
        elif op == 0xf1: self.rd(self.sp); self.a = self.rd(self.sp + 1 & 0xffff); self.sp = self.sp + 2 & 0xffff   # (F isn't kept: the kernel's NMI leaves Z as it was only by this)
        elif op == 0x3a: a = self.imm(); a |= self.imm() << 8; self.a = self.rd(a)
        elif op == 0x32: a = self.imm(); a |= self.imm() << 8; self.wr(a, self.a)
        elif op == 0x3c: self.a = self.a + 1 & 255; self.z = self.a == 0
        elif op == 0x3d: self.a = self.a - 1 & 255; self.z = self.a == 0
        elif op == 0x21: self.l = self.imm(); self.h = self.imm()
        elif op == 0xbe: self.z = self.a == self.rd(self.hl())
        elif op in (0x28, 0x20):
            d = self.imm()
            if self.z == (op == 0x28): bus.idle(5); self.pc = self.pc + (d - 256 if d > 127 else d) & 0xffff
        elif op == 0x7e: self.a = self.rd(self.hl())
        elif op == 0x46: self.b = self.rd(self.hl())
        elif op == 0xdb: p = self.imm(); self.a = self.io_in(self.a, p)
        elif op == 0xd3: p = self.imm(); self.io_out(self.a, p, self.a)
        elif op == 0x26: self.h = self.imm()
        elif op == 0x0e: self.c = self.imm()
        elif op == 0x5f: self.e = self.a
        elif op == 0x57: self.d = self.a
        elif op == 0x6f: self.l = self.a
        elif op == 0x7b: self.a = self.e
        elif op == 0x7a: self.a = self.d
        elif op == 0x07: self.a = (self.a << 1 | self.a >> 7) & 255
        elif op == 0xb3: self.a |= self.e; self.z = self.a == 0
        elif op == 0xe6: self.a &= self.imm(); self.z = self.a == 0
        elif op == 0xf6: self.a |= self.imm(); self.z = self.a == 0
        elif op == 0x23: bus.idle(2); self.set_hl(self.hl() + 1 & 0xffff)
        elif op == 0xc3: a = self.imm(); a |= self.imm() << 8; self.pc = a
        elif op == 0xed:
            op2 = self.m1()
            if op2 == 0x45:                # retn
                lo = self.rd(self.sp); hi = self.rd(self.sp + 1 & 0xffff)
                self.sp = self.sp + 2 & 0xffff; self.pc = hi << 8 | lo
            elif op2 in (0xa3, 0xb3):      # outi, otir
                bus.idle(1)
                v = self.rd(self.hl())
                self.b = self.b - 1 & 255
                self.io_out(self.b, self.c, v)
                self.set_hl(self.hl() + 1 & 0xffff)
                self.z = self.b == 0
                if op2 == 0xb3 and self.b: bus.idle(5); self.pc = self.pc - 2 & 0xffff
            else: raise NotImplementedError('ED %02X at $%04X' % (op2, self.pc - 2))
        else: raise NotImplementedError('%02X at $%04X' % (op, self.pc - 1))
        # (the NMI's handler restores AF: Z as before it)
        if self.pc == 0x803a: self.saved_z = self.z
        if op == 0xed and self.pc != 0x803a and getattr(self, 'saved_z', None) is not None and op2 == 0x45:
            self.z = self.saved_z; self.saved_z = None


# ---- the cart: the plugin's own loop ----------------------------------------------------
class CPU(cs.CPU):
    def step(self):
        pc = self.r[15]
        if pc != cs.RET and not self.it:
            mn, ops, size, _ = self.code[pc]
            b = mn.split('.')[0]
            if b in ('mrs', 'cpsid', 'cpsie'):
                self.r[15] = pc + size; self.t += CYCLE; return
            if b == 'sbfx':
                p = [x.strip() for x in ops.split(',')]
                w = int(p[3][1:])
                v = self.r[self.reg(p[1])] >> int(p[2][1:]) & ((1 << w) - 1)
                if v >> (w - 1) & 1: v |= 0xffffffff << w & 0xffffffff
                self.r[self.reg(p[0])] = v; self.r[15] = pc + size; self.t += CYCLE; return
        super().step()

    def rd(self, a, size=4):
        for base, mem in self.banks:
            if base <= a < base + len(mem):
                if size == 1: return mem[a - base]
                return int.from_bytes(mem[a - base:a - base + 4], 'little')
        return super().rd(a, size)


def run(elf, kernel, lists, seed, bounce=0.0, publish_us=300, quiet=False, psg_wait=29, slack=0.0):
    rng = random.Random(seed)
    bus = cs.Bus('nominal', rng)
    k = Kernel(kernel, bus, lists, rng, psg_wait)
    while not k.ended: k.step()
    for _ in range(40): k.step()
    bus.finish()

    probe = cs.CPU(elf, [])
    new = 'cvf_serve2' in probe.sym
    cpu = CPU(elf, ['cvf_serve2'] if new else ['serve', 'on_select', 'on_header'])
    sym = cpu.sym
    fixed, lram = sym['fixed_ram'][0], sym['list_ram'][0]
    fmem, lmem = bytearray(BANK), bytearray(2 * BANK)
    for a in range(BANK): fmem[mega_index(a)] = kernel[a] if a < len(kernel) else 0xc7
    cpu.banks = [(fixed, fmem), (lram, lmem)]
    if new:
        sv = sym['sv'][0]
        A_READY, A_TAKEN, A_FRAMES, A_REP = sv, sv + 4, sv + 8, sv + 12
        cpu.ram[sv + 16] = fixed
        cpu.ram[sv + 20] = 0
        cpu.r[0] = sv
        entry = sym['cvf_serve2'][0] & ~1
    else:
        A_READY, A_TAKEN, A_FRAMES, A_REP = sym['f_ready'][0], sym['f_taken'][0], sym['f_frames'][0], sym['f_rep'][0]
        entry = sym['serve'][0] & ~1
    cpu.ram[A_TAKEN] = lram
    cpu.ram[A_REP & ~3] = 0x0f030f << (8 * (A_REP & 3))
    cpu.r[13], cpu.r[14], cpu.r[15] = 0x20200000, cs.RET | 1, entry
    T0_US = 5_000_000                      # (past the boot window: a settled $8000 read would be RESET)
    brng = random.Random(seed + 1000)
    falls = sorted(t for t, kk, x in bus.ev if kk == 15 and x == 0)
    chg = [-1e9, None]
    def mmio_r(a):
        if a == cs.SIO_IN:
            ts = cpu.t - 2 * CYCLE         # the input synchroniser
            v = bus.pins(ts)
            if bounce:
                hit = False
                if chg[0] - 5 <= ts <= chg[0] + 30 and not bus.pins(chg[0]) & NCART: hit = brng.random() < 0.6 * bounce
                i = bisect.bisect_right(falls, ts) - 1
                if i >= 0 and ts - falls[i] <= 30 and brng.random() < 0.5 * bounce: hit = True
                if hit:
                    for g in range(16):
                        if brng.random() < 0.2: v ^= 1 << g
            return v
        if a == cs.TIMER: return T0_US + int(cpu.t / 1000)
        raise KeyError(hex(a))
    def mmio_w(a, v):
        assert a == cs.SIO_OUT, hex(a)
        cpu.outs.append((cpu.t + 3 * CYCLE, v >> 16 & 0xff))
        if chg[1] != v >> 16 & 0xff: chg[0], chg[1] = cpu.t + 3 * CYCLE, v >> 16 & 0xff
    cpu.mmio_r, cpu.mmio_w = mmio_r, mmio_w

    # core 0: list f is published publish_us after the console took list f - 1 (the first: at once)
    pubs = []
    for si, f in k.frame_sel:
        if f < len(lists):             # (in time for its frame: this console doesn't run a list again)
            pubs.append((min(bus.selects[k.frame_sel[f - 1][0]][0] + publish_us * 1000, bus.selects[si][0] - 50e3) if f else 0.0, f))
    pubs.sort()
    pi = 0
    takes = []                             # (t, frames, taken bank) whenever the loop counts a frame
    last_frames = 0
    top = min(a for a, c in cpu.code.items() if c[0] == 'ldr' and c[1] == 'r0, [r6, #4]')
    seen = None
    reset_at = None
    try:
        while cpu.t < bus.t:
            if pi < len(pubs) and cpu.t >= pubs[pi][0]:
                f = pubs[pi][1]; pi += 1
                assert not cpu.ram.get(A_READY, 0), 'list %d published before %d was taken' % (f, f - 1)
                which = 0 if cpu.ram[A_TAKEN] != lram else 1
                for i, b in enumerate(lists[f]): lmem[which * BANK + mega_index(i)] = b
                cpu.ram[A_READY] = lram + which * BANK
            if cpu.r[15] == top:           # a quiet bus: whole turns of the main loop at a time
                nxt = bus.times[min(bisect.bisect_right(bus.times, cpu.t), len(bus.times) - 1)]
                if pi < len(pubs): nxt = min(nxt, pubs[pi][0])
                snap = tuple(cpu.r[4:13])
                if seen and seen[0] == snap and nxt - cpu.t > 50000 and bus.pins(cpu.t) & NCART:
                    period = cpu.t - seen[1]
                    n = int((nxt - cpu.t - 2000) / period)
                    if 0 < period < 400 and n > 0: cpu.t += n * period
                seen = (snap, cpu.t)
            cpu.step()
            fr = cpu.ram.get(A_FRAMES, 0)
            if fr != last_frames:
                last_frames = fr
                takes.append((cpu.t, fr, cpu.ram[A_TAKEN]))
    except cs.Stop:
        reset_at = cpu.t
    except KeyError:                       # (cvframes.c's serve calls serve_handoff, which isn't here)
        reset_at = cpu.t

    # every cart read: the right byte from set-up time (less the 74HCT245) until it is sampled
    HCT245 = 25
    ot = [o[0] for o in cpu.outs]
    worst = {'m1': 1e9, 'rd': 1e9}
    bad = []
    limit = reset_at if reset_at is not None else 1e18
    for (ts, setup, a, kind), want in zip(bus.checks, k.exp):
        if ts > limit: break
        if want is None: continue
        need = ts - setup - HCT245
        j = bisect.bisect_right(ot, ts - HCT245) - 1
        i = j
        while i >= 0 and cpu.outs[i][1] == want: i -= 1
        since = ot[i + 1] if i + 1 <= j else None
        margin = need - since if since is not None else -1e9
        worst[kind] = min(worst[kind], margin)
        if margin < -slack: bad.append((ts, a, kind, margin))
    # the selects: each frame's start taken once, in its read, and the list with it; nothing else
    missed = extra = wrong_list = 0
    LATE = 1100
    ti = 0
    for si, f in k.frame_sel:
        fall, rise, n = bus.selects[si]
        if rise + LATE > limit: break
        while ti < len(takes) and takes[ti][0] < fall: extra += 1; ti += 1
        if ti < len(takes) and takes[ti][0] <= rise + LATE:
            ti += 1
        else: missed += 1
    extra += sum(1 for t in takes[ti:] if t[0] < limit)
    false_reset = reset_at is not None
    ok = not bad and not missed and not extra and not false_reset
    reads = sum(1 for w in k.exp if w is not None)
    if not quiet or not ok:
        print('  %s seed %-3d: %d lists, %d cart reads | frame selects missed %d, false %d%s | wrong bytes %d (opcode %d, data %d) | margin: opcode %4.0f ns, data %4.0f ns'
              % ('cvfserve.S' if new else 'cvframes.c', seed, len(lists), reads, missed, extra, ', a false RESET' if false_reset else '',
                 len(bad), sum(1 for b in bad if b[2] == 'm1'), sum(1 for b in bad if b[2] == 'rd'), worst['m1'], worst['rd']))
        for ts, a, kind, m in bad[:int(os.environ.get('NBAD', '4'))]:
            print('      $%04X (%s) at %.1f us: %s' % (a | 0x8000, 'opcode' if kind == 'm1' else 'data', ts / 1000,
                                                      'late by %.0f ns' % -m if m > -1e8 else 'the wrong byte'))
    return ok, len(bad), missed, extra, worst, reads, bad, new


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--kernel')
    ap.add_argument('--lists', help="sw_host's lists.bin (else lists made up in swcv's shapes)")
    ap.add_argument('--first', type=int, default=0, help='the first of --lists to run')
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--seed', type=int, default=None)
    ap.add_argument('--seeds', type=int, default=2)
    ap.add_argument('--bounce', action='store_true')
    ap.add_argument('--hit', type=float, default=1.0)
    ap.add_argument('--publish-us', type=float, default=300)
    ap.add_argument('--slack', type=float, default=None,
                    help='ns a byte may be late by (0; with --bounce 30: the games\' loop, cvserve_sim.py --bounce, is up to 8 late at these odds)')
    ap.add_argument('--expect-bad', action='store_true', help="the run must find wrong bytes (the loop cvfserve.S replaces, with --bounce)")
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    kpath = a.kernel or os.path.join(os.path.dirname(os.path.abspath(a.elf)), 'cvframes', 'gbkernel.bin')
    kernel = open(kpath, 'rb').read()
    fails = tot_bad = tot_reads = 0
    w = {'m1': 1e9, 'rd': 1e9}
    where = {}
    for s in ([a.seed] if a.seed is not None else range(a.seeds)):
        lists = read_lists(a.lists, a.first + s * a.frames, a.frames) if a.lists else made_up_lists(a.frames, random.Random(s + 77))
        ok, nbad, missed, extra, worst, reads, bad, new = run(a.elf, kernel, lists, s, a.hit if a.bounce else 0.0, a.publish_us, a.quiet,
                                                              slack=a.slack if a.slack is not None else 30.0 if a.bounce else 0.0)
        fails += not ok
        tot_bad += nbad; tot_reads += reads
        for kk in w: w[kk] = min(w[kk], worst[kk])
        for ts, ad, kind, m in bad:
            key = 'the kernel' if ad < 0x4000 else 'the list, $%03X-' % ((ad - 0x4000) & ~0xff)
            where[key] = where.get(key, 0) + 1
    print('%s%s: %d cart reads, wrong bytes %d; smallest margin: opcode %.0f ns, data %.0f ns'
          % ('cvfserve.S' if new else 'cvframes.c', ', disturbed pins' if a.bounce else '', tot_reads, tot_bad, w['m1'], w['rd']))
    if where: print('  where: ' + ', '.join('%s %d' % kv for kv in sorted(where.items())))
    if a.expect_bad:
        print('cvframes_sim: %s' % ('wrong bytes, as expected of this loop' if tot_bad else 'NO wrong bytes: the model no longer shows the fault'))
        sys.exit(0 if tot_bad else 1)
    print('cvframes_sim: %s' % ('ok' if not fails else '%d run(s) FAILED' % fails))
    sys.exit(1 if fails else 0)
