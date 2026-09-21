"""Edge-side encoder: depthwise + pointwise convolutions only.

This is what ships to the FPGA and the parameter budget is real (comparability
with MCUCoder), so the architecture is fixed rather than configurable.
"""
from __future__ import annotations

import torch.nn as nn


class ImageEncoder(nn.Module):
    """Depthwise-separable /8 encoder. 11,201 parameters at embedding_dim=64.

    Six blocks of `dw3x3 -> BN -> pw1x1 -> BN -> ReLU6`, widths
    d/4, d/2, d/2, d/2, d, d and strides /2 /2 /2 /1 /1 /1, so /8 is reached by
    block 3 and blocks 4-6 add depth per token.

    Two properties that are load-bearing:

    **One ReLU6 per block, placed AFTER the pointwise.** It therefore only ever
    acts on features that have already been mixed across channels. base_v3 also
    activated between the depthwise and the pointwise, which meant block 0 clipped
    raw per-channel RGB spatial responses at zero before R, G and B had interacted
    at all -- every oriented-gradient-like feature lost its negative lobe before it
    was usable. Fixing it by moving the single activation costs nothing: `dw -> pw`
    with nothing between is just a separable convolution, so there is still one
    nonlinearity per spatial-mixing operation.

    Incidentally, ReLU6's ceiling is free: after BatchNorm activations are ~N(0,1),
    so 6 is a 6-sigma clip that essentially never fires, and it is what bounds the
    tensor for INT8. The floor at zero is the part that costs information, which is
    why there are five of them and not eleven.

    **No activation on the final block.** The quantizer's codebook is initialized
    ~N(0,1) and spans both signs; a trailing ReLU6 would clip the encoder output to
    [0, 6] and make most of the codebook unreachable.
    """

    def __init__(self, in_channels=3, embedding_dim=64):
        super().__init__()
        d = embedding_dim

        def block(cin, cout, stride, act=True):
            layers = [
                nn.Conv2d(cin, cin, 3, stride=stride, padding=1, groups=cin,
                          bias=False),
                nn.BatchNorm2d(cin),
                nn.Conv2d(cin, cout, 1, bias=False),
                nn.BatchNorm2d(cout),
            ]
            if act:
                layers.append(nn.ReLU6(inplace=True))
            return nn.Sequential(*layers)

        # self.encoder = nn.Sequential(
        #             block(in_channels, d // 8, stride=2),      # /2
        #             block(d // 8, d // 4, stride=2),           # /4
        #             block(d // 4, d, stride=2),           # /8
        #             block(d, d, stride=1),
        #             block(d, d, stride=1),
        #             block(d, d, stride=1, act=False),          # -> VQ
        #         )

        self.encoder = nn.Sequential(
            block(in_channels, 16, stride=2),      # /2
            block(16, 48, stride=2),           # /4
            block(48, 64, stride=2, act=False),           # /8
                    # -> VQ
        )

    def forward(self, x):
        return self.encoder(x)

    # ---------------------------------------------------------------- budget
    def n_params(self):
        return sum(p.numel() for p in self.parameters())

    def n_macs(self, image_size=224):
        """Multiply-accumulates for one image -- the FPGA-relevant cost.

        Convs only; BatchNorm folds into the preceding conv at deploy time. Each
        conv is charged at its own output resolution.
        """
        macs, h = 0, image_size
        for m in self.encoder.modules():
            if isinstance(m, nn.Conv2d):
                h //= m.stride[0]
                macs += ((m.in_channels // m.groups) * m.out_channels
                         * m.kernel_size[0] * m.kernel_size[1] * h * h)
        return macs

    def summary(self, image_size=224):
        macs = self.n_macs(image_size)
        return (f"encoder: {self.n_params():,} params | {macs:,} MACs at "
                f"{image_size}px ({macs / image_size ** 2:.0f}/pixel)")
