# Compressor

The model, the training recipe, and the evaluation the reported numbers come
from.

```
vqlic/          the package: encoder, quantizer, decoder, entropy coder, codec
scripts/        entry points -- train, finetune, fit tables, code, evaluate
```

## Install

```bash
pip install -r requirements.txt
pip install -e .            # so `import vqlic` works from anywhere
```

Torch is deliberately not pinned tightly, but note that the INT8 path goes
through `torch.ao.quantization.quantize_fx`, which has moved between minor
versions. If a QAT checkpoint refuses to load with a prepared-encoder mismatch,
that is the first thing to check.

## Paths

Nothing is hardcoded to a machine. Five environment variables, all optional if
you use the default layout:

| variable | what | default |
|---|---|---|
| `VQLIC_WEIGHTS` | checkpoints and context tables | `./weights` |
| `VQLIC_KODAK` | Kodak 24 | `./data/kodak` |
| `VQLIC_CLIC` | CLIC2017 validation | `./data/clic` |
| `VQLIC_TRAIN` | OpenImages train (only for fitting tables) | `./data/train` |
| `VQLIC_BPGENC` / `VQLIC_BPGDEC` | BPG binaries (only for the anchors) | on `PATH` |

Download the weights first — see the [root README](../README.md).

## Code an image

```bash
# round-trip one image: real payload out, decoded back from those bytes
python scripts/codec.py --rung bpp030 --image img.png

# keep the bitstream and the reconstruction
python scripts/codec.py --rung bpp030 --image img.png \
    --write-payload out.bin --write-recon out.png

# decode a payload written earlier -- no original needed
python scripts/codec.py --rung bpp030 --decode out.bin --write-recon out.png

# the FP32 pre-QAT parent, for the INT8-vs-float delta
python scripts/codec.py --rung bpp030 --image img.png --float
```

The three rungs are `bpp010`, `bpp030`, `bpp040`, in ascending rate.

A payload is only decodable by the rung that wrote it: the context tables and
the pruned codebook are the decoder's half of the agreement and are not carried
in the stream. Decoding with the wrong rung does not raise, it produces garbage.

## Reproduce the numbers

```bash
# smoke test first -- two images, one rung
python scripts/eval.py --dataset kodak --rungs bpp030 --limit 2 --tag smoke

# the reported curves, both models, both corpora
python scripts/eval.py --dataset kodak --tag kodak
python scripts/eval.py --dataset clic  --tag clic
```

Writes `ours_<tag>_final.json` (INT8) and `ours_<tag>_float.json` (FP32 parents)
into `--out-dir`. Kodak takes a couple of minutes on a GPU; CLIC is 59 larger
images and takes longer.

Each rung is **verified, not assumed**: after every image the realised indices
are checked against the keep mask, and a single code outside it fails the run
rather than being reported. A violation means the numbers would belong to a
codec other than the one being described.

### The comparison curves

Optional, and they need extra dependencies:

```bash
pip install compressai dahuffman lpips brisque

# classical: JPEG, BPG, MozJPEG. The slowest thing here by a wide margin.
python scripts/eval_classical.py --dataset kodak --tag kodak_full

# learned: Balle hyperprior, Minnen MBT2018, MCUCoder
python scripts/eval_learned.py --dataset kodak --tag kodak_full
```

MCUCoder is not vendored — it has its own license. Clone it and point
`VQLIC_MCU_ROOT` at the `mcu/` directory inside:

```bash
git clone https://github.com/ds-kiel/MCUCoder
```

## Train from scratch

Two stages. The published checkpoints are the output of the second.

```bash
# 1. FP32 rung. --dry-run builds the model and prints its size, touching no data.
python scripts/train.py --dry-run
python scripts/train.py --preset bpp030 --train-dir /data/openimages/train

# 2. INT8 QAT: encoder to INT8, codebook to INT8, prior re-estimated
python scripts/qat.py \
    --qat-from weights/bpp030_fp32.pt \
    --name bpp030_cbint8 --out-dir weights \
    --qat-quantize-codebook --qat-codebook-bits 8 \
    --train-dir /data/openimages/train
```

Order matters in stage 2 and `vqlic/qat.py` sequences it: observers freeze
before the codebook is quantized, and the prior is re-estimated **last**,
against the assignment the INT8 codebook actually produces. A prior estimated
any earlier prices a distribution the deployed model does not emit.

Then refit the context tables, because the re-estimated prior changes which 64
codes survive pruning:

```bash
python scripts/build_ctx_tables.py --rungs bpp030          # INT8 model
python scripts/build_ctx_tables.py --rungs bpp030 --float  # its FP32 parent
```

This needs `VQLIC_TRAIN`. It is the only thing here that does — the fitted
tables ship with the weights.

## How it fits together

```
image
  │
  ├─ encoder ................ ~4.9 KB, INT8, /8 downsample        [on the sensor]
  ├─ quantizer .............. G=4 codebooks x 256 codes, 64 kept  [on the sensor]
  │                           entropy-constrained assignment
  ├─ context model .......... left neighbour -> 65 contexts/group [both peers]
  ├─ rANS ................... 32-bit state, static tables         [on the sensor]
  │
  ▼  payload: header + context-coded index streams
  │
  └─ decoder ................ self-attention synthesis            [receiver]
```

The two halves of the entropy coder are separate files on purpose:
`vqlic/context_fit.py` fits the tables offline over thousands of images;
`vqlic/context.py` codes against them, and is the file the `RTL/`
implementation is checked against.

## Things that will bite you

**`rate_beta` is not in the `state_dict`.** It is a plain attribute and
`build_model` hardcodes `0.0`. Loading a checkpoint without restoring it from
the config swaps the entropy-constrained assignment for plain
nearest-neighbour — which does not raise, does not warn, and inflates the
measured rate by up to 2.6x. Everything here loads through `vqlic/load.py`,
which is the only place that restoration happens. Use it.

**`eval()` is load-bearing.** In train mode the quantizer runs `_ema_update`, a
`@torch.no_grad` function that mutates buffers directly, so wrapping a call site
in `torch.no_grad()` does not stop it and the codebook drifts image to image.

**An INT8 encoder cannot leave the CPU.** fbgemm has no CUDA kernels and moving
a converted module to CUDA segfaults rather than raising. `vqlic.load.place`
splits the model: encoder on the CPU, quantizer and decoder on the GPU.

**A float model and its INT8 finetune need different context tables.** They emit
different code distributions and one table cannot price both. The float tables
carry an `fp32_` infix for exactly this reason.

## The bitstream spec

- [CONTEXT_CODEC.md](CONTEXT_CODEC.md) — normative: what is coded, the context,
  the table format, the payload container, conformance.
- [RANS_GUIDE.md](RANS_GUIDE.md) — implementing the coder: constants, encoder,
  decoder, the width and overflow audit, Cortex-A9 notes.
- [CONTEXT_CODEC_FAQ.md](CONTEXT_CODEC_FAQ.md) — questions from building the
  hardware side.
