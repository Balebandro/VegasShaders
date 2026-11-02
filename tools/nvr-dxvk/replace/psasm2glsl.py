"""psasm2glsl.py: a D3D9 ps_3_0 shader (its fxc /dumpbin disassembly) -> a GLSL replacement for the NVR DXVK fork (phase 2).

  python psasm2glsl.py <shader.asm> <out.frag> <out.txt>

Line for line: every ps_3_0 instruction becomes the same operation in GLSL, so the replacement computes what the original computes
(the starting point a hand-written, unlimited version can grow from). Follows the contract in src/core/NvrDxvkExt.h
(NVR_EXT_SHADER_REPLACEMENT_DESC) and does what DXVK's own conversion of a D3D9 pixel shader adds:
  - D3D9 samplers: set 1 binding i (2D / cube / volume as declared), their sampler from DXVK's heap (push offset 32);
    a sampler with no texture bound (DXVK's spec type 3) reads (0, 0, 0, 1) as on D3D9.
  - float constants: the registers the shader reads (not its def's) in c[] (set 2 binding 6), listed in the .txt as floats=.
  - bool constants: DXVK's spec data (set 3 binding 0, dword 6); def'd ints/bools are literals.
  - the alpha test (spec dword 0, alpha reference in the shared push data), copied from DXVK 3.0.1's fixed-function shader.
  - DXVK's default float emulation ("Enabled"): rcp / rsq / log / pow results clamped to +-FLT_MAX so 0 * inf stays 0.
Not handled (the script stops with a message): loops (rep/loop/aL), predicates (setp/p0), relative addressing, texldp/texldb,
depth-compare (shadow) samplers, oDepth.
"""
import re, sys

FLT_MAX = "3.4028234663852886e38"
COMP = "xyzw"


class Err(Exception): pass


def parse(asm_path):
    lines = []
    for raw in open(asm_path, encoding="utf-8", errors="replace"):
        s = raw.split("//")[0].strip()
        if s: lines.append(s)
    if not lines or not lines[0].startswith("ps_3_0"): raise Err("not a ps_3_0 shader: " + (lines[0] if lines else "empty"))
    return lines[1:]


def full_swizzle(sw):
    if not sw: return "xyzw"
    sw = sw.lower().replace("r", "x").replace("g", "y").replace("b", "z").replace("a", "w")
    return (sw + sw[-1] * 4)[:4]


class Translator:
    def __init__(self):
        self.defs = {}           # c register -> "vec4(...)"
        self.defi = {}           # i register -> ivec4 literal
        self.defb = {}           # b register -> bool literal
        self.used_c = set()
        self.inputs = {}         # v register -> (location, glsl name, centroid)
        self.samplers = {}       # s index -> kind ("2d", "cube", "volume")
        self.temps = set()
        self.outputs = set()
        self.bools = False
        self.vpos = self.vface = False
        self.body = []
        self.depth = 1

    # ---------- operands ----------
    def src(self, tok):
        """A source operand as a vec4 expression (swizzled to four components)."""
        neg = tok.startswith("-")
        if neg: tok = tok[1:]
        m = re.fullmatch(r"([a-zA-Z]+)(\d*)(_abs)?(?:\.([xyzwrgba]+))?", tok)
        if not m:
            if "[" in tok: raise Err("relative addressing is not handled: " + tok)
            raise Err("operand not understood: " + tok)
        kind, num, absmod, sw = m.group(1), m.group(2), m.group(3), m.group(4)
        n = int(num) if num else 0
        if kind == "r": self.temps.add(n); base = "r%d" % n
        elif kind == "c":
            if n in self.defs: base = "k%d" % n
            else: self.used_c.add(n); base = "C(%d)" % n
        elif kind == "v":
            if n not in self.inputs: raise Err("input v%d used but not declared" % n)
            base = self.inputs[n][1]
        elif kind == "vPos": self.vpos = True; base = "vPos"
        elif kind == "vFace": self.vface = True; base = "vFace"
        elif kind == "i":
            if n not in self.defi: raise Err("int constant i%d set by the game is not handled" % n)
            base = "vec4(%s)" % self.defi[n]
        else: raise Err("source register not handled: " + tok)
        e = "%s.%s" % (base, full_swizzle(sw))
        if absmod: e = "abs(%s)" % e
        if neg: e = "(-%s)" % e
        return e

    def scalar_src(self, tok):
        return "(%s).x" % self.src(tok)

    def dst(self, tok):
        m = re.fullmatch(r"([a-zA-Z]+)(\d*)(?:\.([xyzw]+))?", tok)
        if not m: raise Err("destination not understood: " + tok)
        kind, num, mask = m.group(1), m.group(2), m.group(3) or "xyzw"
        n = int(num) if num else 0
        if kind == "r": self.temps.add(n); name = "r%d" % n
        elif kind == "oC": self.outputs.add(n); name = "oC%d" % n
        elif kind == "oDepth": raise Err("oDepth is not handled")
        else: raise Err("destination register not handled: " + tok)
        return name, mask

    def emit(self, s):
        self.body.append("  " * self.depth + s)

    def assign(self, dst_tok, expr4, sat, scalar=False):
        name, mask = self.dst(dst_tok)
        e = "clamp(%s, 0.0, 1.0)" % expr4 if sat else expr4
        if scalar: self.emit("%s.%s = %s(%s);" % (name, mask, "vec%d" % len(mask) if len(mask) > 1 else "", e))
        else: self.emit("%s.%s = (%s).%s;" % (name, mask, e, mask))

    # ---------- declarations ----------
    def dcl(self, op, args):
        parts = op.split("_")
        reg = args[0]
        if reg.startswith("s"):
            kind = parts[1] if len(parts) > 1 else "2d"
            if kind not in ("2d", "cube", "volume"): raise Err("sampler kind not handled: " + op)
            self.samplers[int(reg[1:].split(".")[0])] = kind
            return
        if reg.startswith("vPos"): self.vpos = True; return
        if reg.startswith("vFace"): self.vface = True; return
        if not reg.startswith("v"): raise Err("dcl not handled: %s %s" % (op, reg))
        n = int(re.match(r"v(\d+)", reg).group(1))
        sem = parts[1] if len(parts) > 1 else ""
        centroid = "centroid" in parts
        m = re.fullmatch(r"(texcoord|color|normal|fog)(\d*)", sem)
        if not m: raise Err("input semantic not handled: " + op)
        idx = int(m.group(2) or 0)
        loc = {"texcoord": 1 + idx, "color": 9 + idx, "normal": 0, "fog": 11}[m.group(1)]
        if m.group(1) == "normal" and idx: raise Err("NORMAL%d has no fixed location" % idx)
        self.inputs[n] = (loc, "%s%d" % (m.group(1), idx), centroid)

    # ---------- instructions ----------
    def instr(self, line):
        op, _, rest = line.partition(" ")
        args = [a.strip() for a in rest.split(",")] if rest.strip() else []
        base, *mods = op.split("_")
        sat = "sat" in mods
        if base == "dcl": return self.dcl(op, args)
        if base == "def":
            n = int(args[0][1:]); self.defs[n] = "vec4(%s)" % ", ".join(fmt_float(a) for a in args[1:]); return
        if base == "defi":
            n = int(args[0][1:]); self.defi[n] = ", ".join(args[1:]); return
        if base == "defb":
            n = int(args[0][1:]); self.defb[n] = args[1].lower() == "true"; return
        S = self.src
        if base in ("mov",): return self.assign(args[0], S(args[1]), sat)
        if base == "add": return self.assign(args[0], "%s + %s" % (S(args[1]), S(args[2])), sat)
        if base == "sub": return self.assign(args[0], "%s - %s" % (S(args[1]), S(args[2])), sat)
        if base == "mul": return self.assign(args[0], "%s * %s" % (S(args[1]), S(args[2])), sat)
        if base == "mad": return self.assign(args[0], "%s * %s + %s" % (S(args[1]), S(args[2]), S(args[3])), sat)
        if base == "min": return self.assign(args[0], "min(%s, %s)" % (S(args[1]), S(args[2])), sat)
        if base == "max": return self.assign(args[0], "max(%s, %s)" % (S(args[1]), S(args[2])), sat)
        if base == "frc": return self.assign(args[0], "fract(%s)" % S(args[1]), sat)
        if base == "abs": return self.assign(args[0], "abs(%s)" % S(args[1]), sat)
        if base == "cmp": return self.assign(args[0], "mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))" % (S(args[3]), S(args[2]), S(args[1])), sat)
        if base == "lrp": return self.assign(args[0], "mix(%s, %s, %s)" % (S(args[3]), S(args[2]), S(args[1])), sat)
        if base == "slt": return self.assign(args[0], "vec4(lessThan(%s, %s))" % (S(args[1]), S(args[2])), sat)
        if base == "sge": return self.assign(args[0], "vec4(greaterThanEqual(%s, %s))" % (S(args[1]), S(args[2])), sat)
        if base == "dsx": return self.assign(args[0], "dFdx(%s)" % S(args[1]), sat)
        if base == "dsy": return self.assign(args[0], "dFdy(%s)" % S(args[1]), sat)
        if base == "dp3": return self.assign(args[0], "dot((%s).xyz, (%s).xyz)" % (S(args[1]), S(args[2])), sat, scalar=True)
        if base == "dp4": return self.assign(args[0], "dot(%s, %s)" % (S(args[1]), S(args[2])), sat, scalar=True)
        if base == "dp2add":
            return self.assign(args[0], "dot((%s).xy, (%s).xy) + %s" % (S(args[1]), S(args[2]), self.scalar_src(args[3])), sat, scalar=True)
        if base == "rcp": return self.assign(args[0], "rcpE(%s)" % self.scalar_src(args[1]), sat, scalar=True)
        if base == "rsq": return self.assign(args[0], "rsqE(%s)" % self.scalar_src(args[1]), sat, scalar=True)
        if base == "exp": return self.assign(args[0], "exp2(%s)" % self.scalar_src(args[1]), sat, scalar=True)
        if base == "log": return self.assign(args[0], "logE(%s)" % self.scalar_src(args[1]), sat, scalar=True)
        if base == "pow": return self.assign(args[0], "powE(%s, %s)" % (self.scalar_src(args[1]), self.scalar_src(args[2])), sat, scalar=True)
        if base == "nrm": return self.assign(args[0], "nrmE(%s)" % S(args[1]), sat)
        if base == "crs": return self.assign(args[0], "vec4(cross((%s).xyz, (%s).xyz), 0.0)" % (S(args[1]), S(args[2])), sat)
        if base == "sincos":
            name, mask = self.dst(args[0])
            x = self.scalar_src(args[1])
            for c in mask:
                if c == "x": self.emit("%s.x = %s;" % (name, "clamp(cos(%s), 0.0, 1.0)" % x if sat else "cos(%s)" % x))
                elif c == "y": self.emit("%s.y = %s;" % (name, "clamp(sin(%s), 0.0, 1.0)" % x if sat else "sin(%s)" % x))
            return
        if base in ("texld", "texldl", "texldd"):
            if mods and any(m in ("p", "b") for m in mods): raise Err("texldp / texldb are not handled")
            if line.startswith("texldp") or line.startswith("texldb"): raise Err("texldp / texldb are not handled")
            s = int(args[2][1:].split(".")[0])
            if s not in self.samplers: raise Err("sampler s%d used but not declared" % s)
            kind = self.samplers[s]
            n = {"2d": 2, "cube": 3, "volume": 3}[kind]
            co = "(%s).%s" % (S(args[1]), "xyz"[:n])
            if base == "texld": call = "texture(SMP%d, %s)" % (s, co)
            elif base == "texldl": call = "textureLod(SMP%d, %s, (%s).w)" % (s, co, S(args[1]))
            else: call = "textureGrad(SMP%d, %s, (%s).%s, (%s).%s)" % (s, co, S(args[3]), "xyz"[:n], S(args[4]), "xyz"[:n])
            sw = re.search(r"\.([xyzwrgba]+)$", args[2])
            call = "(isNull(%d) ? vec4(0.0, 0.0, 0.0, 1.0) : %s)" % (s, call)
            if sw: call = "(%s).%s" % (call, full_swizzle(sw.group(1)))
            return self.assign(args[0], call, sat)
        if base == "texkill":
            self.emit("if (any(lessThan((%s).xyz, vec3(0.0)))) discard;" % S(args[0])); return
        if base == "if":
            if len(mods) == 1 and mods[0] in CMP:
                self.emit("if (%s %s %s) {" % (self.scalar_src(args[0]), CMP[mods[0]], self.scalar_src(args[1])))
            elif not mods and args[0].startswith("b"):
                self.emit("if (%s) {" % self.bool_reg(args[0]))
            else: raise Err("if form not handled: " + line)
            self.depth += 1; return
        if base == "else":
            self.depth -= 1; self.emit("} else {"); self.depth += 1; return
        if base == "endif":
            self.depth -= 1; self.emit("}"); return
        if base in ("rep", "loop", "endrep", "endloop", "break", "breakc", "setp", "call", "callnz", "ret", "label"):
            raise Err("flow control not handled: " + base)
        if base == "nop": return
        raise Err("instruction not handled: " + line)

    def bool_reg(self, tok):
        neg = tok.startswith("!")
        n = int(tok.lstrip("!")[1:])
        if n in self.defb: e = "true" if self.defb[n] else "false"
        else: self.bools = True; e = "psBool(%d)" % n
        return "!" + e if neg else e

    def glsl(self, lines):
        for ln in lines: self.instr(ln)
        floats = sorted(self.used_c)
        index = {r: i for i, r in enumerate(floats)}
        body = "\n".join(self.body)
        body = re.sub(r"C\((\d+)\)", lambda m: "c[%d]" % index[int(m.group(1))], body)
        out = ["#version 450",
               "// Generated by tools/nvr-dxvk/replace/psasm2glsl.py: a line-for-line translation of a D3D9 ps_3_0 shader.",
               "#extension GL_EXT_nonuniform_qualifier : enable",
               "#extension GL_EXT_scalar_block_layout : enable",
               "",
               "layout(set = 0, binding = 0) uniform sampler s_samplers[];"]
        for s, kind in sorted(self.samplers.items()):
            out.append("layout(set = 1, binding = %d) uniform %s t%d;" % (s, {"2d": "texture2D", "cube": "textureCube", "volume": "texture3D"}[kind], s))
        if floats: out.append("layout(set = 2, binding = 6, std140) uniform Consts { vec4 c[%d]; };" % len(floats))
        out += ["// D3D9's spec data: DXVK turns each member into a specialization constant, so they must be plain uint members",
                "layout(set = 3, binding = 0, scalar) uniform SpecConsts {",
                "  uint specAlphaTest; uint specFog; uint specFixedFunction; uint specVsSamplers;",
                "  uint specPsSamplerTypes; uint specPsSamplerModes; uint specPsBools; uint specReserved;",
                "};",
                "layout(push_constant) uniform Push {",
                "  uint packedFogColorAndAlphaRef; float fogDistanceScale; float fogDistanceEnd; float fogDensity;",
                "  layout(offset = 32) uint d3d9Samplers[8];",
                "};"]
        for n, (loc, name, centroid) in sorted(self.inputs.items(), key=lambda kv: kv[1][0]):
            out.append("layout(location = %d) %sin vec4 %s;" % (loc, "centroid " if centroid else "", name))
        for n in sorted(self.outputs):
            out.append("layout(location = %d) out vec4 oC%d;" % (n, n))
        out += ["",
                "const float FLT_MAX = %s;" % FLT_MAX,
                "float rcpE(float x) { return min(1.0 / x, FLT_MAX); }",
                "float rsqE(float x) { return min(inversesqrt(abs(x)), FLT_MAX); }",
                "float logE(float x) { return max(log2(abs(x)), -FLT_MAX); }",
                "float powE(float x, float y) { return exp2(y * logE(x)); }",
                "vec4 nrmE(vec4 v) { return v * rsqE(dot(v.xyz, v.xyz)); }",
                "bool isNull(uint s) { return bitfieldExtract(specPsSamplerTypes, int(s + s), 2) == 3u; }",
                "bool psBool(uint b) { return bitfieldExtract(specPsBools, int(b), 1) != 0u; }",
                "uint smp(uint s) { return bitfieldExtract(d3d9Samplers[s >> 1], int((s & 1u) * 16u), 16); }"]
        for s, kind in sorted(self.samplers.items()):
            ctor = {"2d": "sampler2D", "cube": "samplerCube", "volume": "sampler3D"}[kind]
            out.append("#define SMP%d %s(t%d, s_samplers[nonuniformEXT(smp(%du))])" % (s, ctor, s, s))
        out += ["",
                "bool alphaTest(float alpha) {   // DXVK 3.0.1 d3d9_fixed_function_frag.glsl, alphaTest()",
                "  uint compareOp = bitfieldExtract(specAlphaTest, 0, 3);",
                "  uint precisionBits = bitfieldExtract(specAlphaTest, 4, 4);",
                "  uint alphaRefInitial = bitfieldExtract(packedFogColorAndAlphaRef, 24, 8);",
                "  float alphaRef;",
                "  if (compareOp < 7u) {",
                "    if (precisionBits <= 8u) {",
                "      alphaRef = float((alphaRefInitial << precisionBits) | (alphaRefInitial >> (8u - precisionBits)));",
                "      alpha = roundEven(alpha * float((256u << precisionBits) - 1u));",
                "    } else alphaRef = float(alphaRefInitial) / 255.0;",
                "    switch (compareOp) {",
                "      case 1u: return alpha <  alphaRef;",
                "      case 2u: return alpha == alphaRef;",
                "      case 3u: return alpha <= alphaRef;",
                "      case 4u: return alpha >  alphaRef;",
                "      case 5u: return alpha != alphaRef;",
                "      case 6u: return alpha >= alphaRef;",
                "      default: return false;",
                "    }",
                "  }",
                "  return true;",
                "}",
                ""]
        for n, lit in sorted(self.defs.items()):
            out.append("const vec4 k%d = %s;" % (n, lit))
        out += ["", "void main() {"]
        for n in sorted(self.temps): out.append("  vec4 r%d = vec4(0.0);" % n)
        if self.vpos: out.append("  vec4 vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0);")
        if self.vface: out.append("  vec4 vFace = vec4(gl_FrontFacing ? 1.0 : -1.0);")
        for n in sorted(self.outputs): out.append("  oC%d = vec4(0.0);" % n)
        out.append(body)
        if 0 in self.outputs: out.append("  if (!alphaTest(oC0.w)) discard;")
        out.append("}")
        manifest = ["# %s" % "generated by psasm2glsl.py",
                    "textures=" + ",".join(str(s) for s in sorted(self.samplers)),
                    "floats=" + ",".join(str(r) for r in floats)]
        return "\n".join(out) + "\n", "\n".join(manifest) + "\n"


CMP = {"gt": ">", "ge": ">=", "eq": "==", "ne": "!=", "lt": "<", "le": "<="}


def fmt_float(s):
    s = s.strip()
    if s.lower() in ("1.#inf", "inf"): return FLT_MAX
    if s.lower() in ("-1.#inf", "-inf"): return "-" + FLT_MAX
    f = float(s)
    r = repr(f)
    return r if ("." in r or "e" in r) else r + ".0"


def main():
    asm, frag, txt = sys.argv[1:4]
    t = Translator()
    try:
        code, manifest = t.glsl(parse(asm))
    except Err as e:
        print("NOT TRANSLATED: %s" % e); sys.exit(2)
    open(frag, "w", newline="\n").write(code)
    open(txt, "w", newline="\n").write(manifest)
    print("ok: %d instructions -> %s (%d samplers, %d float registers, bools %s)" % (len(t.body), frag, len(t.samplers), len(t.used_c), t.bools))


if __name__ == "__main__":
    main()
