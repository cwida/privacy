#!/usr/bin/env Rscript
# Paper-style unskewed utility figure: full-answer utility plus SAA error decomposition.

user_lib <- Sys.getenv("R_LIBS_USER")
if (user_lib == "") {
  user_lib <- file.path(Sys.getenv("HOME"), "R", "libs")
}
if (!dir.exists(user_lib)) {
  dir.create(user_lib, recursive = TRUE, showWarnings = FALSE)
}
.libPaths(c(user_lib, .libPaths()))

required_packages <- c("ggplot2", "dplyr", "readr", "scales", "systemfonts", "tidyr", "grid")
options(repos = c(CRAN = "https://cloud.r-project.org"))
installed <- rownames(installed.packages())
for (pkg in setdiff(required_packages, "grid")) {
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
  library(grid)
})

base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) "Linux Libertine" else "serif"
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_unskewed_sass_utility_decomposition.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
expected_cols <- c(
  "dataset", "query", "mode", "release", "success", "run", "bound_multiplier", "dp_sass_m",
  "median_error_pct", "saa_estimator_median_error_pct", "saa_sampling_median_error_pct"
)
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
  stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

sass_m_values <- raw %>%
  filter(mode == "dp_sass") %>%
  mutate(dp_sass_m = suppressWarnings(as.integer(dp_sass_m))) %>%
  pull(dp_sass_m)
sass_m_values <- sass_m_values[!is.na(sass_m_values)]
if (length(sass_m_values) == 0) {
  stop("No dp_sass rows with dp_sass_m values found.")
}
target_m <- max(sass_m_values)

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
mechanism_levels <- c("DP standard", "DP elastic", "SAA average", "SAA median")
mechanism_colors <- c(
  "DP standard" = "#95a5a6",
  "DP elastic" = "#a8d4ff",
  "SAA average" = "#009900",
  "SAA median" = "#4dff4d"
)
decomp_levels <- c("Avg mechanism", "Avg sampling", "Median mechanism", "Median sampling")
decomp_colors <- c(
  "Avg mechanism" = "#006d2c",
  "Avg sampling" = "#74c476",
  "Median mechanism" = "#238b45",
  "Median sampling" = "#c7e9c0"
)

normalized <- raw %>%
  mutate(
    dataset = toupper(dataset),
    query = toupper(query),
    success = success_flag(success),
    run = suppressWarnings(as.integer(run)),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    full_error = suppressWarnings(as.numeric(median_error_pct)),
    mechanism_error = suppressWarnings(as.numeric(saa_estimator_median_error_pct)),
    sampling_error = suppressWarnings(as.numeric(saa_sampling_median_error_pct)),
    mechanism = case_when(
      mode == "dp_standard" ~ "DP standard",
      mode == "dp_elastic" ~ "DP elastic",
      mode == "dp_sass" & release == "average" & dp_sass_m == target_m ~ "SAA average",
      mode == "dp_sass" & release == "median" & dp_sass_m == target_m ~ "SAA median",
      TRUE ~ NA_character_
    )
  ) %>%
  filter(dataset == "TPCH", success, !is.na(mechanism), !is.na(bound_multiplier), query %in% query_levels) %>%
  mutate(
    query = factor(query, levels = query_levels),
    mechanism = factor(mechanism, levels = mechanism_levels)
  )

if (nrow(normalized) == 0) {
  stop("No usable TPCH rows left after filtering.")
}

per_config <- normalized %>%
  group_by(query, mechanism, bound_multiplier) %>%
  summarize(
    runs = n(),
    full_error = median(full_error, na.rm = TRUE),
    mechanism_error = median(mechanism_error, na.rm = TRUE),
    sampling_error = median(sampling_error, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  filter(!is.na(full_error), is.finite(full_error))

best_full <- per_config %>%
  group_by(query, mechanism) %>%
  arrange(full_error, .by_group = TRUE) %>%
  slice_head(n = 1) %>%
  ungroup() %>%
  mutate(plot_full_error = pmax(full_error, 0.001))

saa_decomp <- best_full %>%
  filter(mechanism %in% c("SAA average", "SAA median")) %>%
  select(query, mechanism, bound_multiplier, runs, mechanism_error, sampling_error) %>%
  tidyr::pivot_longer(
    cols = c(mechanism_error, sampling_error),
    names_to = "source",
    values_to = "error_pct"
  ) %>%
  mutate(
    source = if_else(source == "mechanism_error", "mechanism", "sampling"),
    series = case_when(
      mechanism == "SAA average" & source == "mechanism" ~ "Avg mechanism",
      mechanism == "SAA average" & source == "sampling" ~ "Avg sampling",
      mechanism == "SAA median" & source == "mechanism" ~ "Median mechanism",
      mechanism == "SAA median" & source == "sampling" ~ "Median sampling",
      TRUE ~ NA_character_
    ),
    series = factor(series, levels = decomp_levels),
    plot_error_pct = pmax(error_pct, 0.001)
  ) %>%
  filter(!is.na(error_pct), is.finite(error_pct))

summary_csv <- file.path(output_dir, paste0("unskewed_sass_utility_decomposition_m", target_m, "_summary.csv"))
readr::write_csv(
  bind_rows(
    best_full %>%
      transmute(query, view = "full_answer", series = as.character(mechanism), bound_multiplier, value = full_error),
    saa_decomp %>%
      transmute(query, view = source, series = as.character(series), bound_multiplier, value = error_pct)
  ),
  summary_csv
)
message("Summary saved to: ", summary_csv)

base_theme <- theme_bw(base_size = 34, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.85),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 16),
    legend.key.size = unit(0.58, "cm"),
    legend.margin = margin(0, 0, -3, 0),
    legend.box.margin = margin(0, 0, -13, 0),
    axis.text.x = element_text(size = 18),
    axis.text.y = element_text(size = 18),
    axis.title = element_text(size = 27),
    plot.title = element_text(size = 24, face = "bold", hjust = 0.5, margin = margin(0, 0, 5, 0)),
    plot.margin = margin(2, 5, 5, 5)
  )

log_breaks <- c(0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000)
log_labels <- c("0.001", "0.01", "0.1", "1", "10", "100", "1K", "10K", "100K")

full_plot <- ggplot(best_full, aes(x = query, y = plot_full_error, fill = mechanism)) +
  geom_col(position = position_dodge2(width = 0.82, preserve = "single"), width = 0.72, na.rm = TRUE) +
  scale_fill_manual(values = mechanism_colors, drop = FALSE) +
  scale_y_log10(breaks = log_breaks, labels = log_labels) +
  guides(fill = guide_legend(nrow = 2, byrow = TRUE)) +
  labs(title = "End-to-end error", x = NULL, y = "median error (%)") +
  base_theme

decomp_plot <- ggplot(saa_decomp, aes(x = query, y = plot_error_pct, fill = series)) +
  geom_col(position = position_dodge2(width = 0.82, preserve = "single"), width = 0.72, na.rm = TRUE) +
  scale_fill_manual(values = decomp_colors, drop = FALSE) +
  scale_y_log10(breaks = log_breaks, labels = log_labels) +
  guides(fill = guide_legend(nrow = 2, byrow = TRUE)) +
  labs(title = "SAA error decomposition", x = NULL, y = NULL) +
  base_theme

plot_file <- file.path(output_dir, paste0("unskewed_sass_utility_decomposition_m", target_m, ".png"))
png(filename = plot_file, width = 5200, height = 1650, res = 350)
grid.newpage()
pushViewport(viewport(layout = grid.layout(1, 2, widths = unit(c(1, 1), "null"))))
print(full_plot, vp = viewport(layout.pos.row = 1, layout.pos.col = 1))
print(decomp_plot, vp = viewport(layout.pos.row = 1, layout.pos.col = 2))
dev.off()
message("Plot saved to: ", plot_file)
