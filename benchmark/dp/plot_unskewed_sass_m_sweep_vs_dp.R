#!/usr/bin/env Rscript
# Paper-style SAA m-sweep against DP baselines on the unskewed TPCH utility run.

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
  stop("Usage: Rscript plot_unskewed_sass_m_sweep_vs_dp.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
expected_cols <- c(
  "dataset", "query", "mode", "release", "success", "run", "bound_multiplier", "dp_sass_m",
  "median_error_pct", "saa_estimator_median_error_pct", "saa_sampling_median_error_pct",
  "saa_noise_scale_median"
)
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
  stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
series_levels <- c("DP standard", "DP elastic", "SAA average", "SAA median")
series_colors <- c(
  "DP standard" = "#95a5a6",
  "DP elastic" = "#a8d4ff",
  "SAA average" = "#009900",
  "SAA median" = "#4dff4d"
)
metric_levels <- c("Full answer error", "SAA mechanism error", "SAA sampling error", "SAA noise scale")

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
    noise_scale = suppressWarnings(as.numeric(saa_noise_scale_median)),
    series = case_when(
      mode == "dp_standard" ~ "DP standard",
      mode == "dp_elastic" ~ "DP elastic",
      mode == "dp_sass" & release == "average" ~ "SAA average",
      mode == "dp_sass" & release == "median" ~ "SAA median",
      TRUE ~ NA_character_
    )
  ) %>%
  filter(dataset == "TPCH", success, query %in% query_levels, !is.na(series), !is.na(bound_multiplier)) %>%
  mutate(
    query = factor(query, levels = query_levels),
    series = factor(series, levels = series_levels)
  )

if (nrow(normalized) == 0) {
  stop("No usable TPCH rows left after filtering.")
}

saa_per_config <- normalized %>%
  filter(series %in% c("SAA average", "SAA median"), !is.na(dp_sass_m)) %>%
  group_by(query, series, dp_sass_m, bound_multiplier) %>%
  summarize(
    full_error = median(full_error, na.rm = TRUE),
    mechanism_error = median(mechanism_error, na.rm = TRUE),
    sampling_error = median(sampling_error, na.rm = TRUE),
    noise_scale = median(noise_scale, na.rm = TRUE),
    runs = n(),
    .groups = "drop"
  ) %>%
  filter(!is.na(full_error), is.finite(full_error))

selected_saa <- saa_per_config %>%
  group_by(query, series, dp_sass_m) %>%
  arrange(full_error, .by_group = TRUE) %>%
  slice_head(n = 1) %>%
  ungroup()

saa_summary <- bind_rows(
  selected_saa %>%
    transmute(query, series, dp_sass_m, metric = "Full answer error", value = full_error),
  selected_saa %>%
    transmute(query, series, dp_sass_m, metric = "SAA mechanism error", value = mechanism_error),
  selected_saa %>%
    transmute(query, series, dp_sass_m, metric = "SAA sampling error", value = sampling_error),
  selected_saa %>%
    transmute(query, series, dp_sass_m, metric = "SAA noise scale", value = noise_scale)
) %>%
  filter(!is.na(value), is.finite(value)) %>%
  group_by(series, dp_sass_m, metric) %>%
  summarize(
    query_count = n(),
    median_value = median(value, na.rm = TRUE),
    p25_value = quantile(value, 0.25, na.rm = TRUE),
    p75_value = quantile(value, 0.75, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(
    metric = factor(metric, levels = metric_levels),
    series = factor(series, levels = series_levels),
    plot_value = pmax(median_value, if_else(metric == "SAA noise scale", 1e-9, 0.001)),
    plot_p25 = pmax(p25_value, if_else(metric == "SAA noise scale", 1e-9, 0.001)),
    plot_p75 = pmax(p75_value, if_else(metric == "SAA noise scale", 1e-9, 0.001))
  )

dp_per_config <- normalized %>%
  filter(series %in% c("DP standard", "DP elastic")) %>%
  group_by(query, series, bound_multiplier) %>%
  summarize(full_error = median(full_error, na.rm = TRUE), runs = n(), .groups = "drop") %>%
  filter(!is.na(full_error), is.finite(full_error))

dp_baselines <- dp_per_config %>%
  group_by(query, series) %>%
  arrange(full_error, .by_group = TRUE) %>%
  slice_head(n = 1) %>%
  ungroup() %>%
  group_by(series) %>%
  summarize(
    query_count = n(),
    baseline = median(full_error, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(
    metric = factor("Full answer error", levels = metric_levels),
    series = factor(series, levels = series_levels),
    plot_baseline = pmax(baseline, 0.001)
  )

summary_csv <- file.path(output_dir, "unskewed_sass_m_sweep_vs_dp_summary.csv")
readr::write_csv(
  bind_rows(
    saa_summary %>%
      transmute(kind = "saa", series = as.character(series), metric = as.character(metric), dp_sass_m,
                value = median_value, p25 = p25_value, p75 = p75_value, query_count),
    dp_baselines %>%
      transmute(kind = "dp_baseline", series = as.character(series), metric = as.character(metric),
                dp_sass_m = NA_integer_, value = baseline, p25 = NA_real_, p75 = NA_real_, query_count)
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
    legend.text = element_text(size = 20),
    legend.key.size = unit(0.70, "cm"),
    legend.margin = margin(0, 0, -4, 0),
    legend.box.margin = margin(0, 0, -14, 0),
    axis.text.x = element_text(size = 18),
    axis.text.y = element_text(size = 18),
    axis.title = element_text(size = 27),
    strip.text = element_text(size = 21, face = "bold", margin = margin(5, 0, 5, 0)),
    strip.background = element_rect(fill = "grey85", color = "grey20", linewidth = 1.0),
    plot.margin = margin(2, 5, 5, 5)
  )

plot_data <- saa_summary %>%
  mutate(dp_sass_m = factor(dp_sass_m, levels = sort(unique(dp_sass_m))))

plot <- ggplot(plot_data, aes(x = dp_sass_m, y = plot_value, color = series, shape = series, group = series)) +
  geom_errorbar(aes(ymin = plot_p25, ymax = plot_p75), width = 0.12, linewidth = 0.75, alpha = 0.80, na.rm = TRUE) +
  geom_line(linewidth = 1.15, na.rm = TRUE) +
  geom_point(size = 3.4, na.rm = TRUE) +
  geom_hline(
    data = dp_baselines,
    aes(yintercept = plot_baseline, color = series),
    linetype = "dashed",
    linewidth = 1.05,
    na.rm = TRUE
  ) +
  facet_wrap(~ metric, nrow = 1, scales = "free_y") +
  scale_color_manual(values = series_colors, drop = FALSE) +
  scale_shape_manual(values = c("DP standard" = NA, "DP elastic" = NA, "SAA average" = 16, "SAA median" = 17),
                     drop = FALSE) +
  scale_y_log10(
    breaks = c(0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, 1e6, 1e9, 1e12),
    labels = c("0.001", "0.01", "0.1", "1", "10", "100", "1K", "10K", "100K", "1M", "1B", "1T")
  ) +
  labs(x = "number of SAA subsamples (m)", y = "lower is better (log scale)") +
  guides(color = guide_legend(nrow = 1, byrow = TRUE), shape = "none") +
  base_theme

plot_file <- file.path(output_dir, "unskewed_sass_m_sweep_vs_dp.png")
png(filename = plot_file, width = 5600, height = 1650, res = 350)
print(plot)
dev.off()
message("Plot saved to: ", plot_file)
