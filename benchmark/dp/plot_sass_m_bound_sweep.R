#!/usr/bin/env Rscript
# Plot DP-SAA m=64 vs m=128 utility across all bound multipliers.

user_lib <- Sys.getenv("R_LIBS_USER")
if (user_lib == "") {
  user_lib <- file.path(Sys.getenv("HOME"), "R", "libs")
}
if (!dir.exists(user_lib)) {
  dir.create(user_lib, recursive = TRUE, showWarnings = FALSE)
}
.libPaths(c(user_lib, .libPaths()))

required_packages <- c("ggplot2", "dplyr", "readr", "scales", "systemfonts")
options(repos = c(CRAN = "https://cloud.r-project.org"))
installed <- rownames(installed.packages())
for (pkg in required_packages) {
  if (!(pkg %in% installed)) {
    message("Installing package: ", pkg)
    install.packages(pkg, dependencies = TRUE, lib = user_lib)
  }
}

suppressPackageStartupMessages({
  library(ggplot2)
  library(dplyr)
  library(readr)
  library(scales)
})

base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) "Linux Libertine" else "serif"
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_sass_m_bound_sweep.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
expected_cols <- c("dataset", "query", "mode", "release", "success", "median_error_pct", "bound_multiplier", "dp_sass_m", "run")
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
  stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}

bound_levels <- c("0.01x", "0.1x", "1x", "10x", "100x")
release_levels <- c("SAA average", "SAA median")
m_values <- sort(unique(as.integer(raw$dp_sass_m[raw$mode == "dp_sass"])))
m_values <- m_values[!is.na(m_values)]
m_levels <- paste0("m=", m_values)
m_colors_all <- c("m=64" = "#009900", "m=128" = "#0072b2", "m=256" = "#d55e00", "m=512" = "#cc79a7")
m_shapes_all <- c("m=64" = 16, "m=128" = 17, "m=256" = 15, "m=512" = 18)
m_colors <- m_colors_all[m_levels]
m_shapes <- m_shapes_all[m_levels]
missing_colors <- is.na(m_colors)
if (any(missing_colors)) {
  fallback_colors <- scales::hue_pal()(sum(missing_colors))
  m_colors[missing_colors] <- fallback_colors
}
missing_shapes <- is.na(m_shapes)
if (any(missing_shapes)) {
  m_shapes[missing_shapes] <- seq(0, sum(missing_shapes) - 1)
}

format_bound <- function(x) {
  dplyr::case_when(
    abs(x - 0.01) < 1e-12 ~ "0.01x",
    abs(x - 0.1) < 1e-12 ~ "0.1x",
    abs(x - 1.0) < 1e-12 ~ "1x",
    abs(x - 10.0) < 1e-12 ~ "10x",
    abs(x - 100.0) < 1e-12 ~ "100x",
    TRUE ~ paste0(format(x, trim = TRUE), "x")
  )
}

normalized <- raw %>%
  mutate(
    dataset = case_when(
      tolower(dataset) == "tpch" ~ "TPC-H",
      tolower(dataset) == "jcch" ~ "JCC-H",
      TRUE ~ toupper(dataset)
    ),
    query = toupper(query),
    success = as.logical(success),
    bound_multiplier = as.numeric(bound_multiplier),
    dp_sass_m = as.integer(dp_sass_m),
    median_error_pct = as.numeric(median_error_pct),
    release_label = case_when(
      mode == "dp_sass" & release == "average" ~ "SAA average",
      mode == "dp_sass" & release == "median" ~ "SAA median",
      TRUE ~ NA_character_
    ),
    m_label = paste0("m=", dp_sass_m),
    bound_label = factor(format_bound(bound_multiplier), levels = bound_levels),
    dataset_query = paste(dataset, query, sep = " ")
  ) %>%
  filter(success, !is.na(release_label), bound_label %in% bound_levels, !is.na(median_error_pct), is.finite(median_error_pct)) %>%
  mutate(
    release_label = factor(release_label, levels = release_levels),
    m_label = factor(m_label, levels = m_levels),
    plot_error_pct = pmax(median_error_pct, 0.001)
  )

if (nrow(normalized) == 0) {
  stop("No DP-SAA m=64/m=128 rows left after filtering.")
}

per_query <- normalized %>%
  group_by(dataset, query, dataset_query, release_label, m_label, bound_label, bound_multiplier) %>%
  summarize(
    runs = n(),
    median_error_pct = median(median_error_pct, na.rm = TRUE),
    plot_error_pct = pmax(median_error_pct, 0.001),
    .groups = "drop"
  )

summary <- per_query %>%
  group_by(release_label, m_label, bound_label, bound_multiplier) %>%
  summarize(
    query_instances = n(),
    median_error_pct = median(median_error_pct, na.rm = TRUE),
    mean_error_pct = mean(median_error_pct, na.rm = TRUE),
    p25_error_pct = quantile(median_error_pct, 0.25, na.rm = TRUE),
    p75_error_pct = quantile(median_error_pct, 0.75, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(
    plot_error_pct = pmax(median_error_pct, 0.001),
    plot_p25_error_pct = pmax(p25_error_pct, 0.001),
    plot_p75_error_pct = pmax(p75_error_pct, 0.001)
  )

summary_csv <- file.path(output_dir, "tpch_jcch_sf30_sass_m_bound_sweep_summary.csv")
readr::write_csv(summary, summary_csv)
message("Summary saved to: ", summary_csv)

base_theme <- theme_bw(base_size = 38, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 27),
    legend.key.size = unit(0.75, "cm"),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -15, 0),
    axis.text.x = element_text(angle = 0, hjust = 0.5, size = 22),
    axis.text.y = element_text(size = 23),
    axis.title = element_text(size = 31),
    strip.text = element_text(size = 25, face = "bold", margin = margin(5, 0, 5, 0)),
    strip.background = element_rect(fill = "grey85", color = "grey20", linewidth = 1.0),
    plot.margin = margin(2, 5, 5, 5)
  )

compact_plot <- ggplot(
  summary,
  aes(x = bound_label, y = plot_error_pct, color = m_label, shape = m_label, group = m_label)
) +
  geom_errorbar(
    aes(ymin = plot_p25_error_pct, ymax = plot_p75_error_pct),
    width = 0.12,
    linewidth = 0.75,
    alpha = 0.85,
    na.rm = TRUE
  ) +
  geom_line(linewidth = 1.2, na.rm = TRUE) +
  geom_point(size = 3.6, na.rm = TRUE) +
  facet_wrap(~ release_label, nrow = 1, scales = "free_y") +
  scale_color_manual(values = m_colors, drop = FALSE) +
  scale_shape_manual(values = m_shapes, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, 1000000),
    labels = c("0.01", "0.1", "1", "10", "100", "1K", "10K", "100K", "1M")
  ) +
  labs(x = "bound multiplier", y = "median error (%)") +
  base_theme

compact_plot_file <- file.path(output_dir, "tpch_jcch_sf30_sass_m_bound_sweep_paper.png")
png(filename = compact_plot_file, width = 5200, height = 1650, res = 350)
print(compact_plot)
dev.off()
message("Compact plot saved to: ", compact_plot_file)

query_plot <- ggplot(
  per_query,
  aes(x = bound_label, y = plot_error_pct, color = m_label, shape = m_label, group = m_label)
) +
  geom_line(linewidth = 0.95, na.rm = TRUE) +
  geom_point(size = 2.4, na.rm = TRUE) +
  facet_grid(release_label + dataset ~ query, scales = "free_y") +
  scale_color_manual(values = m_colors, drop = FALSE) +
  scale_shape_manual(values = m_shapes, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, 1000000),
    labels = c("0.01", "0.1", "1", "10", "100", "1K", "10K", "100K", "1M")
  ) +
  labs(x = "bound multiplier", y = "median error (%)") +
  theme_bw(base_size = 30, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 23),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -12, 0),
    axis.text.x = element_text(angle = 35, hjust = 1, size = 12),
    axis.text.y = element_text(size = 13),
    axis.title = element_text(size = 24),
    strip.text = element_text(size = 14, face = "bold", margin = margin(4, 0, 4, 0)),
    strip.background = element_rect(fill = "grey85", color = "grey20", linewidth = 0.8),
    plot.margin = margin(2, 5, 5, 5)
  )

query_plot_file <- file.path(output_dir, "tpch_jcch_sf30_sass_m_bound_sweep_by_query_paper.png")
png(filename = query_plot_file, width = 7000, height = 4300, res = 450)
print(query_plot)
dev.off()
message("By-query plot saved to: ", query_plot_file)
