# RTL

Hardware implementation of the encode side: the convolutional encoder, the
codebook assignment, and the context-conditioned rANS coder.

Written against the bitstream spec in `Compressor/`, which is normative:

- [../Compressor/CONTEXT_CODEC.md](../Compressor/CONTEXT_CODEC.md) — what is
  coded, the context definition, the table format, the payload container, and
  the conformance requirements.
- [../Compressor/RANS_GUIDE.md](../Compressor/RANS_GUIDE.md) — the coder itself:
  constants, encoder, decoder, the width and overflow audit, Cortex-A9 notes.
- [../Compressor/CONTEXT_CODEC_FAQ.md](../Compressor/CONTEXT_CODEC_FAQ.md)

A stream produced here must decode with `Compressor/scripts/codec.py --decode`
to the same indices, bit for bit. The encoder and the decoder derive the context
id independently — the encoder from the bulk form, the decoder incrementally —
and nothing in the stream flags a disagreement, so that round trip is the check
that matters.
