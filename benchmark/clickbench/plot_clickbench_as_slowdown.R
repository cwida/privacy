#!/usr/bin/env Rscript
# Paper-style ClickBench AS slowdown plot. Input CSV:
# query,mode,median_ms or query,mode,m,median_ms with modes "baseline" and "SIMD-AS".
# Optional third argument selects the SIMD-AS m value to plot; defaults to the largest observed m.

script_file <- sub("^--file=", "", grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "dplyr", "readr", "scales", "stringr", "systemfonts"))

suppressPackageStartupMessages({
  library(ggplot2)
  library(dplyr)
  library(readr)
  library(scales)
  library(stringr)
})

base_font <- PaperFont()

args <- commandArgs(trailingOnly = TRUE)
get_script_dir <- function() {
  ca <- commandArgs(trailingOnly = FALSE)
  fa <- grep("^--file=", ca, value = TRUE)
  if (length(fa) > 0) return(dirname(normalizePath(sub("^--file=", "", fa[1]))))
  getwd()
}

script_dir <- get_script_dir()
input_csv <- if (length(args) >= 1) args[1] else file.path(script_dir, "as_clickbench_results.csv")
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
stopifnot(all(c("query", "mode", "median_ms") %in% colnames(raw)))

if (!("m" %in% colnames(raw))) {
  raw$m <- ifelse(raw$mode == "SIMD-AS", 64L, 0L)
}

rows <- raw %>%
  mutate(
    query = as.character(query),
    mode = as.character(mode),
    m = as.integer(m),
    median_ms = as.numeric(median_ms),
    qnum = as.integer(str_extract(query, "\\d+"))
  ) %>%
  filter(mode %in% c("baseline", "SIMD-AS"), is.finite(median_ms), median_ms > 0, !is.na(qnum))

available_m <- rows %>%
  filter(mode == "SIMD-AS") %>%
  pull(m) %>%
  unique() %>%
  sort()
if (length(available_m) == 0) {
  stop("No SIMD-AS rows found in ", input_csv)
}
plot_m <- if (length(args) >= 3) as.integer(args[3]) else max(available_m)
if (!(plot_m %in% available_m)) {
  stop("Requested m=", plot_m, " is not present in ", input_csv,
       " (available: ", paste(available_m, collapse = ","), ")")
}

baseline_rows <- rows %>%
  filter(mode == "baseline") %>%
  select(query, qnum, baseline = median_ms)

wide <- rows %>%
  filter(mode == "SIMD-AS", m == plot_m) %>%
  select(query, qnum, simd_as = median_ms) %>%
  inner_join(baseline_rows, by = c("query", "qnum")) %>%
  filter(baseline > 0) %>%
  mutate(
    slowdown = simd_as / baseline,
    query_label = paste0("Q", qnum),
    label = case_when(
      qnum == 1 ~ "",
      slowdown >= 100 ~ sprintf("%.0fx", slowdown),
      slowdown >= 10 ~ sprintf("%.0fx", slowdown),
      slowdown >= 3 ~ sprintf("%.1fx", slowdown),
      TRUE ~ ""
    )
  ) %>%
  arrange(qnum)

if (nrow(wide) == 0) {
  stop("No plottable rows found in ", input_csv)
}

geomean <- exp(mean(log(wide$slowdown)))
y_upper <- max(wide$slowdown, na.rm = TRUE) * 1.55

p <- ggplot(wide, aes(x = factor(query_label, levels = query_label), y = slowdown)) +
  geom_hline(yintercept = 1, linewidth = 1.1, color = "#4d4d4d") +
  geom_hline(yintercept = geomean, linewidth = 1.0, linetype = "dashed", color = "#2f7d32") +
  geom_col(width = 0.72, fill = "#4dff4d", color = "#2f7d32", linewidth = 0.2) +
  geom_text(
    data = wide %>% filter(label != ""),
    aes(label = label, y = slowdown * 1.12),
    size = 4.5,
    vjust = 0,
    fontface = "bold",
    family = base_font
  ) +
  annotate(
    "text",
    x = Inf,
    y = geomean * 1.08,
    label = paste0("geomean ", sprintf("%.2fx", geomean)),
    hjust = 1.02,
    vjust = 0,
    size = 5.0,
    family = base_font,
    fontface = "bold",
    color = "#2f7d32"
  ) +
  scale_y_log10(
    breaks = c(1, 2, 4, 8, 16, 32, 64, 128, 256, 512),
    labels = function(x) paste0(x, "x")
  ) +
  coord_cartesian(ylim = c(0.85, y_upper), clip = "off") +
  labs(x = NULL, y = NULL) +
  theme_bw(base_size = 38, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 1.0),
    panel.grid.minor = element_blank(),
    axis.text.x = element_text(angle = 45, hjust = 1, size = 12),
    axis.text.y = element_text(size = 16),
    axis.title = element_blank(),
    plot.title = element_blank(),
    plot.margin = margin(5, 16, 5, 5)
  )

out_file <- file.path(output_dir, "clickbench_as_slowdown_plot_paper.png")
png(filename = out_file, width = 3200, height = 1050, res = 350)
print(p)
dev.off()

message("font: ", base_font)
message("queries plotted: ", nrow(wide))
message("SIMD-AS m plotted: ", plot_m)
message("geomean slowdown: ", sprintf("%.3fx", geomean))
message("Plot saved to: ", out_file)
