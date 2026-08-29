import fs from "node:fs/promises";
import path from "node:path";

const ARTIFACT_TOOL_URL =
  "file:///C:/Users/Fahad/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/dist/artifact_tool.mjs";
const SKIA_CANVAS_URL =
  "file:///C:/Users/Fahad/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/@oai/artifact-tool/node_modules/skia-canvas/lib/index.js";

const {
  Presentation,
  PresentationFile,
  row,
  column,
  grid,
  layers,
  panel,
  text,
  shape,
  rule,
  fill,
  hug,
  fixed,
  wrap,
  grow,
  fr,
  auto,
  drawSlideToCtx,
} = await import(ARTIFACT_TOOL_URL);
const { Canvas } = await import(SKIA_CANVAS_URL);

const ROOT = process.cwd();
const OUTPUT_DIR = path.join(ROOT, "output");
const SCRATCH_DIR = path.join(ROOT, "scratch");
const PREVIEW_DIR = path.join(SCRATCH_DIR, "previews");
const REPORT_PATH = path.join(SCRATCH_DIR, "dw_presentation_report.json");

const SLIDE_W = 1920;
const SLIDE_H = 1080;
const BASE_UNIT = 8;

const COLORS = {
  bg: "#F5F0E6",
  paper: "#FFF9F0",
  ink: "#142033",
  muted: "#5C6777",
  accent: "#0F766E",
  accentSoft: "#D9F0EC",
  ember: "#C96B1B",
  emberSoft: "#F6E3D1",
  steel: "#CAD3DE",
  shell: "#E4EEF9",
  core: "#E7F3EC",
  lineBuf: "#D8EFE2",
  mac: "#DCE7FA",
  ppu: "#F6E3D1",
};

const TITLE_FONT = "Bahnschrift";
const BODY_FONT = "Aptos";
const CODE_FONT = "Consolas";

function titleStyle(overrides = {}) {
  return {
    fontFamily: TITLE_FONT,
    fontSize: 46,
    bold: true,
    color: COLORS.ink,
    ...overrides,
  };
}

function bodyStyle(overrides = {}) {
  return {
    fontFamily: BODY_FONT,
    fontSize: 24,
    color: COLORS.ink,
    ...overrides,
  };
}

function smallStyle(overrides = {}) {
  return {
    fontFamily: BODY_FONT,
    fontSize: 16,
    color: COLORS.muted,
    ...overrides,
  };
}

function codeStyle(overrides = {}) {
  return {
    fontFamily: CODE_FONT,
    fontSize: 20,
    color: COLORS.ink,
    ...overrides,
  };
}

function solidLine(color, width = 1) {
  return { style: "solid", width, fill: color };
}

function pill(label, fillColor, textColor = COLORS.ink) {
  return panel(
    {
      width: hug,
      height: hug,
      fill: fillColor,
      line: solidLine(fillColor, 0),
      borderRadius: "rounded-full",
      padding: { x: 18, y: 10 },
    },
    text(label, {
      width: hug,
      height: hug,
      style: bodyStyle({
        fontSize: 18,
        bold: true,
        color: textColor,
      }),
    }),
  );
}

function bulletList(items, options = {}) {
  const width = options.width ?? fill;
  const fontSize = options.fontSize ?? 24;
  const color = options.color ?? COLORS.ink;
  return column(
    {
      width,
      height: hug,
      gap: options.gap ?? 12,
      align: "start",
    },
    items.map((item) =>
      text("- " + item, {
        width,
        height: hug,
        style: bodyStyle({
          fontSize,
          color,
          lineSpacing: 1.15,
        }),
      }),
    ),
  );
}

function labelValue(label, value, options = {}) {
  return column(
    {
      width: options.width ?? fill,
      height: hug,
      gap: 6,
      align: "start",
    },
    [
      text(label, {
        width: fill,
        height: hug,
        style: smallStyle({
          fontSize: 15,
          bold: true,
          color: COLORS.muted,
        }),
      }),
      text(value, {
        width: fill,
        height: hug,
        style: options.code ? codeStyle({ fontSize: 22 }) : bodyStyle({ fontSize: 24 }),
      }),
    ],
  );
}

function statBlock(value, label, fillColor) {
  return panel(
    {
      width: fill,
      height: fill,
      fill: fillColor,
      line: solidLine(fillColor, 0),
      borderRadius: "rounded-xl",
      padding: 28,
    },
    column(
      { width: fill, height: fill, justify: "between", gap: 12 },
      [
        text(value, {
          width: fill,
          height: hug,
          style: titleStyle({
            fontSize: 42,
            color: COLORS.ink,
          }),
        }),
        text(label, {
          width: fill,
          height: hug,
          style: bodyStyle({
            fontSize: 20,
            color: COLORS.muted,
          }),
        }),
      ],
    ),
  );
}

function modulePanel(title, subtitle, fillColor, options = {}) {
  return panel(
    {
      width: fixed(options.width ?? 210),
      height: fixed(options.height ?? 210),
      fill: fillColor,
      line: solidLine(COLORS.steel, 1),
      borderRadius: "rounded-xl",
      padding: options.padding ?? 22,
    },
    column(
      { width: fill, height: fill, justify: "between", gap: 12 },
      [
        text(title, {
          width: fill,
          height: hug,
          style: titleStyle({
            fontSize: options.titleSize ?? 24,
          }),
        }),
        text(subtitle, {
          width: fill,
          height: hug,
          style: bodyStyle({
            fontSize: options.bodySize ?? 18,
            color: COLORS.muted,
            lineSpacing: 1.1,
          }),
        }),
      ],
    ),
  );
}

function miniCard(title, lines, fillColor = COLORS.paper) {
  return panel(
    {
      width: fill,
      height: hug,
      fill: fillColor,
      line: solidLine(COLORS.steel, 1),
      borderRadius: "rounded-xl",
      padding: 20,
    },
    column(
      { width: fill, height: hug, gap: 12 },
      [
        text(title, {
          width: fill,
          height: hug,
          style: titleStyle({ fontSize: 24 }),
        }),
        ...lines.map((line) =>
          text(line, {
            width: fill,
            height: hug,
            style: bodyStyle({
              fontSize: 18,
              color: COLORS.ink,
              lineSpacing: 1.1,
            }),
          }),
        ),
      ],
    ),
  );
}

function windowCell(value, fillColor, textColor = COLORS.ink) {
  return panel(
    {
      width: fill,
      height: fill,
      fill: fillColor,
      line: solidLine(COLORS.steel, 1),
      borderRadius: "rounded-md",
      padding: 8,
      align: "center",
      justify: "center",
    },
    text(value, {
      width: fill,
      height: hug,
      style: codeStyle({
        fontSize: 20,
        bold: true,
        color: textColor,
        textAlign: "center",
      }),
    }),
  );
}

function slideHeader(titleText, subtitleText, tagItems = []) {
  return column(
    {
      width: fill,
      height: hug,
      gap: 18,
      align: "start",
    },
    [
      row(
        {
          width: fill,
          height: hug,
          justify: "between",
          align: "start",
          gap: 24,
        },
        [
          column(
            { width: grow(1), height: hug, gap: 10, align: "start" },
            [
              text(titleText, {
                name: "slide-title",
                width: fill,
                height: hug,
                style: titleStyle({ fontSize: 48 }),
              }),
              text(subtitleText, {
                name: "slide-subtitle",
                width: wrap(1220),
                height: hug,
                style: bodyStyle({
                  fontSize: 25,
                  color: COLORS.muted,
                  lineSpacing: 1.15,
                }),
              }),
            ],
          ),
          row(
            { width: hug, height: hug, gap: 10, justify: "end", align: "start" },
            tagItems,
          ),
        ],
      ),
      rule({
        width: fixed(220),
        weight: 4,
        stroke: COLORS.accent,
      }),
    ],
  );
}

function footer(textValue) {
  return text(textValue, {
    name: "source",
    width: fill,
    height: hug,
    style: smallStyle({
      fontSize: 13,
      color: COLORS.muted,
    }),
  });
}

function fullSlide(content) {
  return layers(
    { width: fill, height: fill },
    [
      shape({
        width: fill,
        height: fill,
        fill: COLORS.bg,
      }),
      content,
    ],
  );
}

function compose(slide, content) {
  slide.compose(content, {
    frame: { left: 0, top: 0, width: SLIDE_W, height: SLIDE_H },
    baseUnit: BASE_UNIT,
  });
}

await fs.mkdir(OUTPUT_DIR, { recursive: true });
await fs.mkdir(PREVIEW_DIR, { recursive: true });

const presentation = Presentation.create({
  slideSize: { width: SLIDE_W, height: SLIDE_H },
});

// Slide 1: cover
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      layers(
        { width: fill, height: fill },
        [
          row(
            {
              width: fill,
              height: fill,
              gap: 0,
            },
            [
              shape({
                width: fixed(340),
                height: fill,
                fill: COLORS.accentSoft,
              }),
              shape({
                width: grow(1),
                height: fill,
                fill: COLORS.bg,
              }),
            ],
          ),
          row(
            {
              width: fill,
              height: fill,
              padding: { x: 110, y: 92 },
              gap: 54,
              align: "start",
            },
            [
              column(
                {
                  width: fixed(700),
                  height: fill,
                  justify: "between",
                  gap: 24,
                },
                [
                  column(
                    { width: fill, height: hug, gap: 18 },
                    [
                      row({ width: hug, height: hug, gap: 12 }, [
                        pill("RTL deep dive", COLORS.accent, "#FFFFFF"),
                        pill("Academic viva", COLORS.emberSoft),
                      ]),
                      text("DW module", {
                        width: wrap(620),
                        height: hug,
                        style: titleStyle({
                          fontSize: 98,
                          color: COLORS.ink,
                        }),
                      }),
                      text("Depthwise wrapper plus shared compute core", {
                        width: wrap(620),
                        height: hug,
                        style: bodyStyle({
                          fontSize: 30,
                          color: COLORS.accent,
                          bold: true,
                        }),
                      }),
                    ],
                  ),
                  column(
                    { width: fill, height: hug, gap: 12 },
                    [
                      text("How the depthwise path streams pixels, creates 3x3 windows, accumulates MACs, and only finishes when the last AXI output beat is accepted.", {
                        width: wrap(650),
                        height: hug,
                        style: bodyStyle({
                          fontSize: 30,
                          color: COLORS.muted,
                          lineSpacing: 1.18,
                        }),
                      }),
                      text("RTL anchors: dw_plane_run_axis.sv, compute_engine.sv, line_buffer.sv, conv_mac_array.sv, ppu.sv", {
                        width: fill,
                        height: hug,
                        style: smallStyle({
                          fontSize: 15,
                          color: COLORS.muted,
                        }),
                      }),
                    ],
                  ),
                ],
              ),
              column(
                {
                  width: fixed(760),
                  height: fill,
                  justify: "between",
                  gap: 28,
                },
                [
                  shape({
                    width: fill,
                    height: fixed(1),
                    fill: COLORS.bg,
                  }),
                  panel(
                    {
                      width: fill,
                      height: hug,
                      fill: COLORS.paper,
                      line: solidLine(COLORS.steel, 1),
                      borderRadius: "rounded-xl",
                      padding: 28,
                    },
                    column(
                      { width: fill, height: hug, gap: 16 },
                      [
                        text("Cover thesis", {
                          width: fill,
                          height: hug,
                          style: titleStyle({
                            fontSize: 28,
                            color: COLORS.accent,
                          }),
                        }),
                        text("dw_plane_run_axis is the shell that makes the accelerator behave like a stream endpoint. The real arithmetic path is inside compute_engine, which this shell locks into depthwise mode.", {
                          width: fill,
                          height: hug,
                          style: bodyStyle({
                            fontSize: 23,
                            lineSpacing: 1.15,
                          }),
                        }),
                      ],
                    ),
                  ),
                  panel(
                    {
                      width: fill,
                      height: hug,
                      fill: COLORS.emberSoft,
                      line: solidLine(COLORS.steel, 1),
                      borderRadius: "rounded-xl",
                      padding: 28,
                    },
                    column(
                      { width: fill, height: hug, gap: 12 },
                      [
                        text("Presentation promise", {
                          width: fill,
                          height: hug,
                          style: titleStyle({
                            fontSize: 24,
                            color: COLORS.ember,
                          }),
                        }),
                        text("We will follow one pixel from AXI ingress to AXI egress, showing exactly where buffering, window legality, MAC validity, quantization, TLAST, and external completion are decided.", {
                          width: fill,
                          height: hug,
                          style: bodyStyle({
                            fontSize: 21,
                            lineSpacing: 1.14,
                          }),
                        }),
                      ],
                    ),
                  ),
                ],
              ),
            ],
          ),
        ],
      ),
    ),
  );
}

// Slide 2: end-to-end datapath
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 34,
        },
        [
          slideHeader(
            "DW datapath in one glance",
            "The wrapper owns stream adaptation and output accounting. The core owns padded window generation, convolution, and post-processing.",
            [pill("shell / core split", COLORS.accentSoft)],
          ),
          layers(
            {
              width: fill,
              height: fixed(320),
            },
            [
              row(
                {
                  width: fill,
                  height: hug,
                  gap: 28,
                  align: "center",
                  justify: "center",
                  padding: { y: 36 },
                },
                [
                  modulePanel("AXI input", "MM2S byte stream\ns_axis_tvalid / tready", COLORS.shell, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                  text("->", {
                    width: hug,
                    height: hug,
                    style: titleStyle({ fontSize: 34, color: COLORS.accent }),
                  }),
                  modulePanel("Input FIFO", "Decouples DMA from real pixel consumption", COLORS.shell, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                  text("->", {
                    width: hug,
                    height: hug,
                    style: titleStyle({ fontSize: 34, color: COLORS.accent }),
                  }),
                  modulePanel("line_buffer", "Pads internally and emits valid 3x3 windows", COLORS.lineBuf, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                  text("->", {
                    width: hug,
                    height: hug,
                    style: titleStyle({ fontSize: 34, color: COLORS.accent }),
                  }),
                  modulePanel("conv_mac_array", "9 signed products plus adder tree", COLORS.mac, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                  text("->", {
                    width: hug,
                    height: hug,
                    style: titleStyle({ fontSize: 34, color: COLORS.accent }),
                  }),
                  modulePanel("ppu", "Bias, requantize, clamp, optional ReLU", COLORS.ppu, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                  text("->", {
                    width: hug,
                    height: hug,
                    style: titleStyle({ fontSize: 34, color: COLORS.accent }),
                  }),
                  modulePanel("Output FIFO", "Stores {tlast,data}\nand absorbs backpressure", COLORS.shell, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                  text("->", {
                    width: hug,
                    height: hug,
                    style: titleStyle({ fontSize: 34, color: COLORS.accent }),
                  }),
                  modulePanel("AXI output", "S2MM stream\nm_axis_tvalid / tready / tlast", COLORS.shell, {
                    width: 180,
                    height: 210,
                    titleSize: 21,
                    bodySize: 16,
                    padding: 18,
                  }),
                ],
              ),
              row(
                {
                  width: fill,
                  height: hug,
                  justify: "between",
                  align: "start",
                  padding: { x: 150, y: 0 },
                },
                [
                  panel(
                    {
                      width: fixed(516),
                      height: hug,
                      fill: COLORS.shell,
                      line: solidLine(COLORS.steel, 1),
                      borderRadius: "rounded-full",
                      padding: { x: 18, y: 10 },
                    },
                    text("Wrapper region: dw_plane_run_axis", {
                      width: fill,
                      height: hug,
                      style: bodyStyle({
                        fontSize: 18,
                        bold: true,
                        color: COLORS.ink,
                        textAlign: "center",
                      }),
                    }),
                  ),
                  panel(
                    {
                      width: fixed(640),
                      height: hug,
                      fill: COLORS.core,
                      line: solidLine(COLORS.steel, 1),
                      borderRadius: "rounded-full",
                      padding: { x: 18, y: 10 },
                    },
                    text("Shared compute_engine configured for depthwise mode", {
                      width: fill,
                      height: hug,
                      style: bodyStyle({
                        fontSize: 18,
                        bold: true,
                        color: COLORS.ink,
                        textAlign: "center",
                      }),
                    }),
                  ),
                  shape({
                    width: fixed(516),
                    height: fixed(1),
                    fill: COLORS.bg,
                  }),
                ],
              ),
            ],
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(1.2), fr(1), fr(1)],
              columnGap: 32,
              alignItems: "stretch",
            },
            [
              miniCard("Wrapper decisions", [
                "s_axis_tready goes low when the input FIFO is full or start_in is flushing state.",
                "throttle_in halts core intake when the output FIFO approaches full.",
                "m_axis_tlast is stored per output pixel, not computed on the fly at the port.",
              ]),
              miniCard("Core configuration for DW", [
                "is_depthwise = 1",
                "bypass_1x1 = 0",
                "mode_residual = 0",
                "psum_clear = 1 and residual inputs are tied off",
              ], COLORS.core),
              miniCard("What the audience should remember", [
                "The shell guarantees correct stream behavior.",
                "The core guarantees correct math over a virtual padded image.",
                "The two are coupled by FIFO contracts rather than direct AXI timing.",
              ], COLORS.emberSoft),
            ],
          ),
          footer("Slide claim verified against dw_plane_run_axis.sv and compute_engine.sv."),
        ],
      ),
    ),
  );
}

// Slide 3: wrapper contract
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 30,
        },
        [
          slideHeader(
            "What dw_plane_run_axis actually owns",
            "It is the stream shell around a shared compute engine: parameter capture, FIFO decoupling, output counting, TLAST tagging, and external completion.",
            [pill("top-level contract", COLORS.emberSoft)],
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(1.1), fr(0.9)],
              rows: [auto, fr(1)],
              columnGap: 34,
              rowGap: 24,
            },
            [
              panel(
                {
                  width: fill,
                  height: hug,
                  fill: COLORS.paper,
                  line: solidLine(COLORS.steel, 1),
                  borderRadius: "rounded-xl",
                  padding: 26,
                },
                grid(
                  {
                    width: fill,
                    height: hug,
                    columns: [fr(1), fr(1)],
                    columnGap: 24,
                    rowGap: 20,
                  },
                  [
                    labelValue("Control", "start_in, done_out"),
                    labelValue("Geometry", "img_width, img_height, pad_top, stride_2"),
                    labelValue("Quantization", "zp_in, zp_out, bias_in, mult_in, shift_in"),
                    labelValue("Kernel", "weights_3x3[2:0][2:0]", { code: true }),
                    labelValue("Ingress AXI", "s_axis_tdata / tvalid / tready / tlast"),
                    labelValue("Egress AXI", "m_axis_tdata / tvalid / tready / tlast"),
                  ],
                ),
              ),
              panel(
                {
                  width: fill,
                  height: hug,
                  fill: COLORS.shell,
                  line: solidLine(COLORS.steel, 1),
                  borderRadius: "rounded-xl",
                  padding: 26,
                },
                column(
                  { width: fill, height: hug, gap: 14 },
                  [
                    text("Wrapper-specific control logic", {
                      width: fill,
                      height: hug,
                      style: titleStyle({ fontSize: 26 }),
                    }),
                    bulletList(
                      [
                        "Derived out_w / out_h determine how many pixels must be emitted.",
                        "produced_cnt marks the last generated pixel before it enters the output FIFO.",
                        "done_out does not follow core_done; it waits for final AXI acceptance.",
                      ],
                      { width: fill, fontSize: 18 },
                    ),
                  ],
                ),
              ),
              panel(
                {
                  width: fill,
                  height: fill,
                  fill: COLORS.paper,
                  line: solidLine(COLORS.steel, 1),
                  borderRadius: "rounded-xl",
                  padding: 28,
                },
                column(
                  { width: fill, height: fill, gap: 24 },
                  [
                    text("Input side behavior", {
                      width: fill,
                      height: hug,
                      style: titleStyle({ fontSize: 28 }),
                    }),
                    bulletList(
                      [
                        "s_axis_tready = !in_full && !start_in",
                        "Input FIFO uses show-ahead read: pixel_in always sees in_mem[in_rd_ptr].",
                        "in_pop happens only when consume_in && valid_in, so padding does not advance the FIFO.",
                      ],
                      { width: fill, fontSize: 21 },
                    ),
                    text("Output side behavior", {
                      width: fill,
                      height: hug,
                      style: titleStyle({ fontSize: 28, color: COLORS.accent }),
                    }),
                    bulletList(
                      [
                        "Each output FIFO entry stores {last, data} as a 9-bit word.",
                        "throttle_in rises when out_count >= OUT_FIFO_DEPTH - 32.",
                        "done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast.",
                      ],
                      { width: fill, fontSize: 21 },
                    ),
                  ],
                ),
              ),
              panel(
                {
                  width: fill,
                  height: fill,
                  fill: COLORS.emberSoft,
                  line: solidLine(COLORS.steel, 1),
                  borderRadius: "rounded-xl",
                  padding: 28,
                },
                column(
                  { width: fill, height: fill, justify: "between", gap: 18 },
                  [
                    text("One subtle but important note", {
                      width: fill,
                      height: hug,
                      style: titleStyle({
                        fontSize: 28,
                        color: COLORS.ember,
                      }),
                    }),
                    text("The wrapper contains an explicit comment that its stride-2 output size formula is a ceil(W/2), ceil(H/2) approximation for the network being targeted. That is acceptable for the current use case, but it is still a model-level assumption worth stating in a viva.", {
                      width: fill,
                      height: hug,
                      style: bodyStyle({
                        fontSize: 23,
                        lineSpacing: 1.16,
                      }),
                    }),
                    text("Presentation takeaway: the shell is not mathematically deep, but it decides what the outside world believes the accelerator has completed.", {
                      width: fill,
                      height: hug,
                      style: bodyStyle({
                        fontSize: 20,
                        bold: true,
                        color: COLORS.ink,
                      }),
                    }),
                  ],
                ),
              ),
            ],
          ),
          footer("Verified against dw_plane_run_axis.sv: FIFO control, output counting, TLAST generation, and top-level done_out."),
        ],
      ),
    ),
  );
}

// Slide 4: line buffer
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 30,
        },
        [
          slideHeader(
            "line_buffer turns a 1D stream into a padded 3x3 view",
            "Padding is synthesized internally with zp_in, so the input FIFO only advances when a real source pixel is needed.",
            [pill("window generator", COLORS.lineBuf)],
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(1.15), fr(0.85)],
              columnGap: 34,
            },
            [
              column(
                { width: fill, height: fill, gap: 24 },
                [
                  panel(
                    {
                      width: fill,
                      height: hug,
                      fill: COLORS.paper,
                      line: solidLine(COLORS.steel, 1),
                      borderRadius: "rounded-xl",
                      padding: 26,
                    },
                    column(
                      { width: fill, height: hug, gap: 18 },
                      [
                        text("Internal storage structure", {
                          width: fill,
                          height: hug,
                          style: titleStyle({ fontSize: 28 }),
                        }),
                        row(
                          { width: fill, height: hug, gap: 18 },
                          [
                            miniCard("line1 BRAM", ["Two rows back at the current column tap"], COLORS.paper),
                            miniCard("line0 BRAM", ["Previous row at the current column tap"], COLORS.paper),
                            miniCard("Shift registers", ["s0_x, s1_x, s2_x hold the current 3-column neighborhood"], COLORS.paper),
                          ],
                        ),
                      ],
                    ),
                  ),
                  panel(
                    {
                      width: fill,
                      height: fill,
                      fill: COLORS.lineBuf,
                      line: solidLine(COLORS.steel, 1),
                      borderRadius: "rounded-xl",
                      padding: 26,
                    },
                    column(
                      { width: fill, height: fill, gap: 20 },
                      [
                        text("How a window is formed on each advance", {
                          width: fill,
                          height: hug,
                          style: titleStyle({ fontSize: 28 }),
                        }),
                        grid(
                          {
                            width: fixed(520),
                            height: fixed(360),
                            columns: [fr(1), fr(1), fr(1)],
                            rows: [fr(1), fr(1), fr(1)],
                            columnGap: 10,
                            rowGap: 10,
                          },
                          [
                            windowCell("s0_0", COLORS.paper),
                            windowCell("s0_1", COLORS.paper),
                            windowCell("s0_2", COLORS.paper),
                            windowCell("s1_0", COLORS.paper),
                            windowCell("s1_1", COLORS.emberSoft),
                            windowCell("s1_2", COLORS.paper),
                            windowCell("s2_0", COLORS.paper),
                            windowCell("s2_1", COLORS.paper),
                            windowCell("s2_2", COLORS.accentSoft),
                          ],
                        ),
                        text("At every advance: s2_2 gets px_gen, s1_2 gets line0[gen_col], and s0_2 gets line1[gen_col]. Then line0 stores px_gen and line1 stores the old line0 value.", {
                          width: wrap(950),
                          height: hug,
                          style: bodyStyle({
                            fontSize: 22,
                            lineSpacing: 1.12,
                          }),
                        }),
                      ],
                    ),
                  ),
                ],
              ),
              column(
                { width: fill, height: fill, gap: 22 },
                [
                  miniCard("Key predicates", [
                    "need_real_px: current generated coordinate lies inside the real image bounds",
                    "advance: running && (!need_real_px || valid_in)",
                    "consume_in: advance && need_real_px",
                  ], COLORS.paper),
                  miniCard("What that means architecturally", [
                    "Padding cells do not consume FIFO bytes.",
                    "Real cells stall until valid_in is present.",
                    "The line buffer owns the exact pacing contract for the core.",
                  ], COLORS.accentSoft),
                  miniCard("When does lb_valid_out rise?", [
                    "Only after enough state exists to define a legal output center.",
                    "For stride_2, output_valid_check keeps only even center_r / center_c locations.",
                    "For this DW wrapper, bypass_1x1 is 0, so the full 3x3 path is always used.",
                  ], COLORS.paper),
                  miniCard("Internal done signal", [
                    "line_buffer done_out pulses when the final padded coordinate is processed.",
                    "That is a core-completion event, not the same thing as the wrapper's stream completion.",
                  ], COLORS.emberSoft),
                ],
              ),
            ],
          ),
          footer("Verified against line_buffer.sv: padded virtual image generation, consume_in semantics, output_valid_check, and line RAM update order."),
        ],
      ),
    ),
  );
}

// Slide 5: MAC
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 28,
        },
        [
          slideHeader(
            "conv_mac_array performs the actual depthwise convolution",
            "For DW mode the block uses all nine taps, subtracts zp_in per activation, and ignores psum_in entirely.",
            [pill("2-stage MAC", COLORS.mac)],
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(0.95), fr(1.05)],
              columnGap: 34,
            },
            [
              panel(
                {
                  width: fill,
                  height: fill,
                  fill: COLORS.paper,
                  line: solidLine(COLORS.steel, 1),
                  borderRadius: "rounded-xl",
                  padding: 28,
                },
                column(
                  { width: fill, height: fill, gap: 18 },
                  [
                    text("Depthwise datapath equation", {
                      width: fill,
                      height: hug,
                      style: titleStyle({ fontSize: 30 }),
                    }),
                    text("mac_out = sum over 3x3 [ (window[r][c] - zp_in) * weights[r][c] ]", {
                      width: wrap(760),
                      height: hug,
                      style: codeStyle({
                        fontSize: 28,
                        color: COLORS.accent,
                      }),
                    }),
                    text("The pointwise accumulation path exists in the shared module, but the DW wrapper forces is_depthwise = 1 and psum_clear = 1, so the partial-sum path is logically bypassed.", {
                      width: fill,
                      height: hug,
                      style: bodyStyle({
                        fontSize: 22,
                        lineSpacing: 1.12,
                      }),
                    }),
                    grid(
                      {
                        width: fixed(560),
                        height: fixed(360),
                        columns: [fr(1), fr(1), fr(1)],
                        rows: [fr(1), fr(1), fr(1)],
                        columnGap: 12,
                        rowGap: 12,
                      },
                      [
                        windowCell("(w00 - zp) * k00", COLORS.shell),
                        windowCell("(w01 - zp) * k01", COLORS.shell),
                        windowCell("(w02 - zp) * k02", COLORS.shell),
                        windowCell("(w10 - zp) * k10", COLORS.shell),
                        windowCell("(w11 - zp) * k11", COLORS.accentSoft),
                        windowCell("(w12 - zp) * k12", COLORS.shell),
                        windowCell("(w20 - zp) * k20", COLORS.shell),
                        windowCell("(w21 - zp) * k21", COLORS.shell),
                        windowCell("(w22 - zp) * k22", COLORS.shell),
                      ],
                    ),
                  ],
                ),
              ),
              column(
                { width: fill, height: fill, gap: 22 },
                [
                  miniCard("Stage 1: registered products", [
                    "Nine signed 18-bit product registers p00_r ... p22_r are captured in always_ff.",
                    "This maps naturally onto DSP multiply registers rather than leaving all products combinational.",
                    "valid_s1, is_dw_s1, psum_clear_s1, and psum_in_s1 pipeline the control alongside the data.",
                  ], COLORS.paper),
                  miniCard("Stage 2: balanced adder tree", [
                    "Products are grouped into s0 ... s4 and then reduced into tree_sum.",
                    "For DW, mac_next = tree_sum.",
                    "For non-DW, only p11_r is used and psum_in can be accumulated.",
                  ], COLORS.mac),
                  miniCard("Pipeline meaning", [
                    "lb_valid_out marks a valid 3x3 window.",
                    "mac_valid_out appears after the product-register stage and output-register stage have done their work.",
                    "The design is throughput-oriented: once windows arrive regularly, a new MAC result can emerge every cycle.",
                  ], COLORS.paper),
                ],
              ),
            ],
          ),
          footer("Verified against conv_mac_array.sv: depthwise tap selection, registered products, balanced adder tree, and psum handling."),
        ],
      ),
    ),
  );
}

// Slide 6: PPU
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 28,
        },
        [
          slideHeader(
            "PPU converts accumulator output back to 8-bit pixels",
            "In the DW wrapper the residual path is disabled, but the shared PPU still carries both conv and residual machinery.",
            [pill("4-stage post-processing", COLORS.ppu)],
          ),
          row(
            {
              width: fill,
              height: fixed(360),
              gap: 22,
              align: "stretch",
            },
            [
              statBlock("Stage 1", "acc + bias, then multiply by mult_conv", COLORS.paper),
              statBlock("Stage 2", "absolute value, sign extraction, path selection", COLORS.accentSoft),
              statBlock("Stage 3", "right shift with ties-to-even rounding, sign restore, zp_out add", COLORS.emberSoft),
              statBlock("Stage 4", "clamp to [0,255], then optional ReLU floor at zp_out", COLORS.paper),
            ],
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(1.1), fr(0.9)],
              columnGap: 34,
            },
            [
              panel(
                {
                  width: fill,
                  height: fill,
                  fill: COLORS.paper,
                  line: solidLine(COLORS.steel, 1),
                  borderRadius: "rounded-xl",
                  padding: 28,
                },
                column(
                  { width: fill, height: fill, gap: 16 },
                  [
                    text("Quantization equation, conceptually", {
                      width: fill,
                      height: hug,
                      style: titleStyle({ fontSize: 28 }),
                    }),
                    text("q = RoundTiesToEven( (conv_acc_in + bias_in) * mult_conv >> shift_conv ) + zp_out", {
                      width: wrap(880),
                      height: hug,
                      style: codeStyle({
                        fontSize: 26,
                        color: COLORS.accent,
                      }),
                    }),
                    text("The hardware separates sign handling from magnitude handling. Stage 2 forms an absolute-value path, stage 3 performs the barrel shift and rounding on magnitude, then the sign is restored before zero-point addition.", {
                      width: fill,
                      height: hug,
                      style: bodyStyle({
                        fontSize: 22,
                        lineSpacing: 1.14,
                      }),
                    }),
                    text("That split matters because the 68-bit negate and shift logic is one of the architectural hotspots in the local timing analysis.", {
                      width: wrap(840),
                      height: hug,
                      style: bodyStyle({
                        fontSize: 22,
                        bold: true,
                        color: COLORS.ember,
                      }),
                    }),
                  ],
                ),
              ),
              column(
                { width: fill, height: fill, gap: 20 },
                [
                  miniCard("Residual path status in DW mode", [
                    "mode_residual = 0 in the DW wrapper.",
                    "res_a_in, res_b_in, mult_res_a, mult_res_b, shift_res, zp_in_a, and zp_in_b are tied off.",
                    "So ppu_trigger follows mac_valid_out, not raw valid_in.",
                  ], COLORS.paper),
                  miniCard("ReLU status", [
                    "relu_en = 0 in dw_plane_run_axis.",
                    "The ReLU hardware still exists in the shared PPU, but it is inactive for this DW shell instance.",
                  ], COLORS.accentSoft),
                  miniCard("Output contract", [
                    "PPU valid_out only means a quantized pixel exists for the wrapper.",
                    "The wrapper still has to store it, tag tlast, and wait for AXI acceptance.",
                  ], COLORS.emberSoft),
                ],
              ),
            ],
          ),
          footer("Verified against ppu.sv: 4-stage conv path, ties-to-even rounding, zero-point add, clamp, and ReLU gating."),
        ],
      ),
    ),
  );
}

// Slide 7: control and timing
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 28,
        },
        [
          slideHeader(
            "Control timing: valid is not the same as done",
            "The most defensible way to explain control is to follow one real pixel through the contracts that transform it into one accepted output beat.",
            [pill("handshake semantics", COLORS.accentSoft)],
          ),
          panel(
            {
              width: fill,
              height: fixed(360),
              fill: COLORS.paper,
              line: solidLine(COLORS.steel, 1),
              borderRadius: "rounded-xl",
              padding: 24,
            },
            row(
              {
                width: fill,
                height: fill,
                gap: 14,
                justify: "between",
                align: "center",
              },
                [
                modulePanel("1. in_push", "s_axis_tvalid && s_axis_tready\nwrites FIFO", COLORS.shell, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("2. valid_in", "FIFO non-empty and not throttled", COLORS.shell, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("3. consume_in", "line_buffer needs a real pixel now", COLORS.lineBuf, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("4. lb_valid_out", "A legal 3x3 center exists", COLORS.lineBuf, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("5. mac_valid_out", "The MAC pipeline has finished this window", COLORS.mac, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("6. valid_out", "PPU has produced an 8-bit pixel", COLORS.ppu, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("7. out_push", "Wrapper stores {tlast,data}", COLORS.shell, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
                modulePanel("8. done_out", "Final beat is accepted on AXI output", COLORS.emberSoft, {
                  width: 170,
                  height: 210,
                  titleSize: 18,
                  bodySize: 14,
                  padding: 18,
                }),
              ],
            ),
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(1), fr(1), fr(1)],
              columnGap: 28,
            },
            [
              miniCard("Why consume_in matters", [
                "consume_in is the only signal that proves a source byte was truly consumed by the core.",
                "Padding positions self-generate zp_in and therefore do not pop the input FIFO.",
              ], COLORS.paper),
              miniCard("Why valid_out is not enough", [
                "PPU valid_out only means the wrapper may push the output FIFO.",
                "If the DMA sink stalls, the final pixel can exist internally long before external completion is visible.",
              ], COLORS.accentSoft),
              miniCard("Why wrapper done_out is strong", [
                "done_out = m_axis_tvalid && m_axis_tready && m_axis_tlast",
                "That definition means the module reports completion only after the outside world has actually accepted the last pixel.",
              ], COLORS.emberSoft),
            ],
          ),
          footer("Verified against dw_plane_run_axis.sv, compute_engine.sv, and line_buffer.sv: in_push, valid_in, consume_in, valid_out chain, TLAST tagging, and done_out semantics."),
        ],
      ),
    ),
  );
}

// Slide 8: critique and defense points
{
  const slide = presentation.slides.add();
  compose(
    slide,
    fullSlide(
      column(
        {
          width: fill,
          height: fill,
          padding: { x: 92, y: 72 },
          gap: 28,
        },
        [
          slideHeader(
            "Design critique and viva defense points",
            "This slide turns the RTL into an engineering argument: what is already sound, where the bottlenecks are, and what tradeoffs the current implementation has accepted.",
            [pill("viva critique", COLORS.emberSoft)],
          ),
          row(
            {
              width: fill,
              height: fixed(250),
              gap: 24,
              align: "stretch",
            },
            [
              statBlock("32 entries", "Throttle margin before output FIFO saturation", COLORS.paper),
              statBlock("Core done != stream done", "line_buffer completion is intentionally weaker than wrapper completion", COLORS.accentSoft),
              statBlock("~40-65 MHz", "Local estimate for current bottlenecked Fmax from existing analysis notes", COLORS.emberSoft),
            ],
          ),
          grid(
            {
              width: fill,
              height: fill,
              columns: [fr(1), fr(1), fr(1)],
              columnGap: 28,
            },
            [
              miniCard("Strengths worth defending", [
                "Clear shell/core separation keeps stream semantics outside the arithmetic core.",
                "consume_in cleanly distinguishes padding from real pixel fetches.",
                "Top-level done_out is externally meaningful because it is acceptance-based.",
              ], COLORS.paper),
              miniCard("Known limitations", [
                "The wrapper comment explicitly admits an approximate stride-2 output-size rule for the current network assumptions.",
                "The compute_engine instance produces an internal core_done that the wrapper does not expose.",
                "Throughput is likely limited by the PPU post-processing path rather than the wrapper FIFOs.",
              ], COLORS.accentSoft),
              miniCard("Natural improvement path", [
                "Pipeline the PPU further if timing becomes the dominant issue.",
                "Parameterize image-width limits instead of hard-wiring 2048 in line_buffer.",
                "If the system grows, make the wrapper's size formula exact and document the supported convolution cases.",
              ], COLORS.emberSoft),
            ],
          ),
          footer("Critique points grounded in dw_plane_run_axis.sv and the existing compute_engine_analysis.md timing report."),
        ],
      ),
    ),
  );
}

const pptxBlob = await PresentationFile.exportPptx(presentation);
const pptxPath = path.join(OUTPUT_DIR, "output.pptx");
await pptxBlob.save(pptxPath);

const previewFiles = [];

for (let i = 0; i < presentation.slides.items.length; i += 1) {
  const slide = presentation.slides.items[i];
  const canvas = new Canvas(SLIDE_W, SLIDE_H);
  const ctx = canvas.getContext("2d");
  await drawSlideToCtx(slide, presentation, ctx);
  const previewPath = path.join(PREVIEW_DIR, `slide-${String(i + 1).padStart(2, "0")}.png`);
  await canvas.toFile(previewPath);
  previewFiles.push(previewPath);
}

await fs.writeFile(
  REPORT_PATH,
  JSON.stringify(
    {
      pptxPath,
      previewFiles,
      slideCount: presentation.slides.items.length,
      generatedAt: new Date().toISOString(),
    },
    null,
    2,
  ),
);

console.log(JSON.stringify({ pptxPath, previewFiles, reportPath: REPORT_PATH }, null, 2));
