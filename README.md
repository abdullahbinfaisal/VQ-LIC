# Neural Image Codec for Edge Hardware

A learned image codec sized for a microcontroller-class encoder: a ~4.9 KB
convolutional encoder, a multi-codebook vector quantizer, and a
context-conditioned rANS coder. The whole encode side fits in about 23 KB of
ROM — 4.9 KB of encoder weights, a 4.1 KB pruned codebook, and 14.2 KB of
context tables. The decoder is a self-attention synthesis network and runs on
the receiving side, where the compute budget is not the constraint.

The asymmetry is the point. Almost all of the arithmetic sits in the decoder, so
the sensor only has to run the encoder and the entropy coder.

```
RTL/            hardware implementation of the encoder and the rANS coder
Compressor/     the model, training, and the evaluation it is measured by
```

## Results

Kodak, 24 images, full resolution. Rate is the **measured payload** in every row
-- `8 * len(bitstream) / pixels` -- never an entropy estimate.

| rung | bpp | PSNR (dB) | MS-SSIM (dB) |
|---|---|---|---|
| bpp010 | 0.0654 | 23.12 | 8.38 |
| bpp030 | 0.1768 | 25.61 | 11.44 |
| bpp040 | 0.2158 | 25.99 | 12.04 |

That is the deployed configuration: INT8 encoder, INT8 codebook, 64 of 256 codes
kept per codebook, and left-neighbour context coding.

Against **MCUCoder**, the closest edge-targeted learned codec, at matched
quality on Kodak:

| | bpp | PSNR (dB) |
|---|---|---|
| ours (bpp030) | 0.1768 | 25.61 |
| MCUCoder (3 channels) | 0.2194 | 25.02 |

— about 19% less rate at 0.6 dB higher PSNR.

Classical codecs still win on PSNR at these rates: BPG at `q=42` reaches 28.7 dB
for 0.1606 bpp. A 4.9 KB encoder does not beat a mature transform codec on
fidelity, and this one does not claim to. The comparison it is built for is
against what else can run on the sensor.

CLIC2017 validation (59 images) is reported alongside Kodak; see
[Compressor/README.md](Compressor/README.md).

## Model weights

The checkpoints and the fitted context tables are hosted separately:

**→ [Download weights](TODO: add the Hugging Face or Drive link here)**

Unpack them into `Compressor/weights/`, or point `VQLIC_WEIGHTS` at wherever
they live. Six checkpoints (three rungs x FP32 parent + INT8 QAT finetune) and
the context tables, about 160 MB in total.

Nothing in the repo will silently run without them: every script resolves its
weights through one function that fails with the path it looked for.

## Getting started

```bash
cd Compressor
pip install -r requirements.txt
pip install -e .

# round-trip one image and print what it cost
python scripts/codec.py --rung bpp030 --image path/to/image.png
```

[Compressor/README.md](Compressor/README.md) covers training, the INT8 finetune,
refitting the context tables, and reproducing the numbers above.

## The bitstream

The encoder and the decoder must agree on the context definition token for
token, or a stream decodes to silently different indices with nothing in it to
flag the disagreement. Three documents specify that contract, and the `RTL/`
implementation is written against them:

- [Compressor/CONTEXT_CODEC.md](Compressor/CONTEXT_CODEC.md) — the normative
  spec: what is coded, the context, the table format, the payload container,
  and the conformance requirements.
- [Compressor/RANS_GUIDE.md](Compressor/RANS_GUIDE.md) — an implementation guide
  for the coder itself, including the width and overflow audit and Cortex-A9
  notes.
- [Compressor/CONTEXT_CODEC_FAQ.md](Compressor/CONTEXT_CODEC_FAQ.md) — the
  questions that came up while building the hardware side.

## License

MIT. See [LICENSE](LICENSE).

MCUCoder, CompressAI, BPG and MozJPEG are third-party baselines used only for
comparison; they are not vendored here and carry their own licenses.
