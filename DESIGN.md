---
name: Mistlane documentation
description: A readable bilingual engineering manual built on VitePress.
colors:
  primary: "#246553"
  primary-hover: "#317c66"
  background: "#fcfcf9"
  background-alt: "#f1f3ed"
  background-soft: "#f2f4ef"
  text: "#23352e"
  text-secondary: "#53635c"
  divider: "#d7dfd6"
  code-background: "#edf1eb"
  dark-primary: "#9bd1bb"
  dark-background: "#19211e"
  dark-background-alt: "#141c18"
  dark-background-soft: "#222e28"
  dark-text: "#e6eee7"
  dark-text-secondary: "#b6c7bb"
  dark-divider: "#394b40"
  dark-code-background: "#121c16"
typography:
  headline:
    fontSize: "clamp(30px, 3vw, 40px)"
    lineHeight: 1.22
    letterSpacing: "-0.025em"
  body:
    fontFamily: "'Segoe UI', 'PingFang SC', 'Microsoft YaHei', sans-serif"
    fontSize: "16px"
    lineHeight: 1.85
  diagram-label:
    fontFamily: "system-ui, sans-serif"
    fontSize: "15px"
  label:
    fontSize: "13px"
rounded:
  diagram: "12px"
  control: "6px"
spacing:
  diagram-gap: "28px"
  toolbar-block: "10px"
  toolbar-inline: "16px"
  diagram-block: "24px"
components:
  diagram-control:
    textColor: "{colors.primary}"
    rounded: "{rounded.control}"
    padding: "3px 10px"
  diagram-toolbar:
    backgroundColor: "{colors.background-soft}"
    textColor: "{colors.text-secondary}"
    padding: "10px 16px"
---

# Design System: Mistlane documentation

## Overview

**Creative North Star: "Engineering manual" (provisional execution choice).**

Mode: Read. The implemented theme prioritizes long-form technical reading and legible architecture diagrams. It extends the standard VitePress theme, preserving navigation, search, locale routes, language switching, accessibility behavior, and code blocks. The optional aesthetic preference remains unanswered; this document records the implementation, not user approval of the palette or a permanent brand identity.

**Key Characteristics:**
- Quiet reading surfaces with green links and clear text contrast in light and dark modes.
- System fonts, compact diagram controls, and locally scrollable technical content.
- Standard VitePress page structure with separate English and Chinese routes.

## Colors

### Primary

Deep green identifies links, diagram controls, and keyboard focus. Dark mode changes to a lighter green. Background, hover, and active color variables remain defined in `docs/.vitepress/theme/custom.css`; frontmatter records the principal reused values, not every VitePress variable.

### Neutral

The light reading surface is off-white with a quieter sidebar, soft toolbar/table-header surfaces, ink-green text, and subtle dividers. Dark mode uses distinct dark surfaces and pale text. Code blocks have their own background in each mode.

Mermaid has a separate theme-aware palette in `docs/.vitepress/theme/mermaid.ts`, including node fills, borders, lines, clusters, and label colors. It is not an identical palette across light and dark modes.

## Typography

The body uses Segoe UI with Chinese system-font fallbacks and no external font request. Body text has generous leading; mobile body text becomes 15px. Main headings use the fluid headline token with balanced wrapping. Second-level headings have 48px top margins and 24px top padding, with the margin reduced to 36px on mobile; third-level headings have 32px top margins. Tables use 14px text with 1.65 leading, reduced to 13px on mobile. The site title uses 18px, weight 700; active sidebar text uses weight 650. Body numerals are tabular.

Mermaid labels use the separate system-ui font at 15px. Toolbar and source-disclosure labels use the label token; source text uses 12px.

## Layout

The sidebar width is 264px and the layout maximum is 1512px. At viewport widths of at least 960px, documents with an aside have a 760px content maximum. The mobile typography breakpoint is 640px. Table cells use 12px by 16px padding. Scroll padding at the top of the page is 96px.

Diagrams have 28px vertical margins. Their toolbar uses a flexible row with a 12px gap and the padding tokens above. The canvas has 24px by 16px padding and horizontal overflow contained within the diagram. Source code scrolls locally as needed.

## Elevation & Depth

The custom theme adds no shadows. Surface colors, one-pixel dividers, and borders distinguish the diagram toolbar, reading surface, and source disclosure. Inherited VitePress elements retain the default theme's depth behavior.

## Shapes

Diagram containers have gently rounded 12px corners and clip their contents. Diagram buttons use 6px corners and one-pixel divider-colored borders. Other VitePress component shapes are inherited.

## Components

### Navigation and code

Retain standard VitePress navigation, search, locale switching, and code-block behavior. Theme overrides adjust the title, sidebar spacing, active-item weight, colors, and reading width without introducing a custom navigation shell.

### Diagram controls and source disclosure

A compact toolbar identifies the architecture diagram and offers an expand toggle. The button exposes its state through `aria-pressed`, with English or Chinese labels based on the current locale. Hover fills the control with the reading-surface color. Global keyboard focus uses a two-pixel accent outline with four-pixel offset.

Diagrams default to fitting the page at all viewport sizes, including mobile. Expansion removes the SVG maximum-width constraint, restoring its natural width; the canvas then offers local horizontal scrolling. The canvas is keyboard focusable and has a localized accessible label. A native disclosure always provides the Mermaid source. Loading uses a status message; failure uses an alert and retains source access.

### Mermaid rendering

Rendering starts after client mount, dynamically imports Mermaid, and waits for `document.fonts.ready`. A shared promise queue serializes renders and recovers after failures. Each render uses a unique ID; stale results are ignored after a newer request or component unmount. Theme and source changes request a fresh render.

Mermaid uses strict security, the base theme, classic appearance, and dagre layout. HTML labels are disabled both at the root (`htmlLabels: false`) and in flowchart configuration. Flowcharts use natural width, 28px node spacing, and 38px rank spacing; sequence diagrams use natural width and wrapped labels.

Reduced-motion preference disables smooth scrolling and reduces transition/animation durations to 0.01ms in the custom stylesheet. No raster imagery ships with this treatment; review screenshots remain local ignored artifacts.

## Do's and Don'ts

### Do:
- Do keep the theme tokens and this document aligned with the shipped CSS and Mermaid configuration.
- Do preserve page-fit diagrams on mobile and make expanded diagrams scroll within their own canvas.
- Do retain localized labels, source disclosure, keyboard focus, and theme-aware diagrams.

### Don't:
- Don't describe this provisional palette as a user-approved brand decision.
- Don't replace the standard locale routes or navigation to express this reading theme.
- Don't treat local review screenshots as production assets.
