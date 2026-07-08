#!/usr/bin/env Rscript
# DP bound-quality utility plotter.
# Reads dp_benchmark_runner CSV output, writes a summarized plotting CSV, and
# produces paper-style plots for TPC-H/JCC-H bound-quality sweeps.

user_lib <- Sys.getenv("R_LIBS_USER")
if (user_lib == "") {
  user_lib <- file.path(Sys.getenv("HOME"), "R", "libs")
}
if (!dir.exists(user_lib)) {
  dir.create(user_lib, recursive = TRUE, showWarnings = FALSE)
}
.libPaths(c(user_lib, .libPaths()))

required_packages <- c("ggplot2", "dplyr", "readr", "scales", "stringr", "systemfonts")
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
  library(stringr)
})

base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) "Linux Libertine" else "serif"
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_bound_quality.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))

expected_cols <- c(
  "dataset", "query", "mode", "release", "success", "median_error_pct",
  "recall", "bound_multiplier", "dp_sum_bound", "dp_count_bound",
  "dp_max_groups_contributed", "dp_sass_sum_output_bound", "run"
)
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
  stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}

scenario_levels <- c("terrible tight", "bad tight", "good", "bad loose", "terrible loose")
scenario_breaks <- c(0.01, 0.1, 1.0, 10.0, 100.0)

mechanism_levels <- c("DP standard", "DP elastic", "SASS median", "SASS average", "SASS bounded ratio")
mechanism_colors <- c(
  "DP standard" = "#95a5a6",
  "DP elastic" = "#a8d4ff",
  "SASS median" = "#4dff4d",
  "SASS average" = "#009900",
  "SASS bounded ratio" = "#005f2f"
)

plot_data <- raw %>%
  mutate(
    dataset = toupper(dataset),
    query = toupper(query),
    run = as.integer(run),
    bound_multiplier = as.numeric(bound_multiplier),
    median_error_pct = as.numeric(median_error_pct),
    recall = as.numeric(recall),
    mechanism = case_when(
      mode == "dp_standard" ~ "DP standard",
      mode == "dp_elastic" ~ "DP elastic",
      mode == "dp_sass" & release == "median" ~ "SASS median",
      mode == "dp_sass" & release == "average" ~ "SASS average",
      mode == "dp_sass_bounded_ratio" ~ "SASS bounded ratio",
      TRUE ~ mode
    ),
    scenario = case_when(
      abs(bound_multiplier - 0.01) < 1e-12 ~ "terrible tight",
      abs(bound_multiplier - 0.1) < 1e-12 ~ "bad tight",
      abs(bound_multiplier - 1.0) < 1e-12 ~ "good",
      abs(bound_multiplier - 10.0) < 1e-12 ~ "bad loose",
      abs(bound_multiplier - 100.0) < 1e-12 ~ "terrible loose",
      TRUE ~ paste0(bound_multiplier, "x")
    ),
    usable = success & !is.na(recall) & recall > 0 & !is.na(median_error_pct)
  ) %>%
  filter(mechanism %in% mechanism_levels, scenario %in% scenario_levels) %>%
  mutate(
    mechanism = factor(mechanism, levels = mechanism_levels),
    scenario = factor(scenario, levels = scenario_levels),
    query = factor(query, levels = c("Q01", "Q05", "Q06", "Q14", "Q19"))
  )

if (nrow(plot_data) == 0) {
  stop("No rows left after filtering mechanisms and bound-quality scenarios.")
}

summary_df <- plot_data %>%
  group_by(dataset, query, mechanism, scenario) %>%
  summarize(
    runs = n(),
    usable_runs = sum(usable),
    suppressed_runs = sum(!usable),
    median_error_pct = ifelse(usable_runs > 0, median(median_error_pct[usable], na.rm = TRUE), NA_real_),
    p25_error_pct = ifelse(usable_runs > 0, quantile(median_error_pct[usable], 0.25, na.rm = TRUE), NA_real_),
    p75_error_pct = ifelse(usable_runs > 0, quantile(median_error_pct[usable], 0.75, na.rm = TRUE), NA_real_),
    median_time_ms = ifelse(usable_runs > 0, median(time_ms[usable], na.rm = TRUE), NA_real_),
    dp_sum_bound = first(dp_sum_bound),
    dp_count_bound = first(dp_count_bound),
    c_u = first(dp_max_groups_contributed),
    dp_sass_sum_output_bound = first(dp_sass_sum_output_bound),
    .groups = "drop"
  ) %>%
  mutate(
    plot_error_pct = pmax(median_error_pct, 0.001, na.rm = TRUE),
    plot_error_pct = ifelse(is.na(median_error_pct), NA_real_, plot_error_pct)
  )

summary_csv <- file.path(output_dir, "tpch_jcch_sf30_bound_quality_plot_data.csv")
readr::write_csv(summary_df, summary_csv)
message("Summary CSV saved to: ", summary_csv)

make_plot <- function(df, dataset_name, out_file) {
  df <- df %>% filter(dataset == dataset_name)
  if (nrow(df) == 0) {
    message("No rows for dataset ", dataset_name, "; skipping.")
    return(invisible(NULL))
  }

  p <- ggplot(df, aes(x = scenario, y = plot_error_pct, color = mechanism, group = mechanism)) +
    geom_line(linewidth = 1.2, na.rm = TRUE) +
    geom_point(size = 3.2, na.rm = TRUE) +
    facet_wrap(~ query, nrow = 1) +
    scale_color_manual(values = mechanism_colors, name = NULL) +
    scale_y_log10(
      breaks = c(0.01, 1, 100, 10000, 1000000),
      labels = c("0.01", "1", "100", "10K", "1M")
    ) +
    labs(x = NULL, y = "median error (%)") +
    theme_bw(base_size = 40, base_family = base_font) +
    theme(
      panel.border = element_rect(linewidth = 1.0),
      panel.grid.major = element_line(linewidth = 1.0),
      panel.grid.minor = element_blank(),
      legend.position = "top",
      legend.title = element_blank(),
      legend.text = element_text(size = 21),
      legend.margin = margin(0, 0, -5, 0),
      legend.box.margin = margin(0, 0, -20, 0),
      axis.text.x = element_text(angle = 35, hjust = 1, size = 15),
      axis.text.y = element_text(size = 20),
      axis.title = element_text(size = 30),
      strip.text = element_text(size = 24, face = "bold"),
      plot.title = element_blank(),
      plot.margin = margin(2, 5, 5, 5)
    )

  png(filename = out_file, width = 5200, height = 1600, res = 350)
  print(p)
  dev.off()
  message("Plot saved to: ", out_file)
}

make_combined_plot <- function(df, out_file) {
  if (nrow(df) == 0) {
    message("No rows for combined plot; skipping.")
    return(invisible(NULL))
  }

  p <- ggplot(df, aes(x = scenario, y = plot_error_pct, color = mechanism, group = mechanism)) +
    geom_line(linewidth = 1.2, na.rm = TRUE) +
    geom_point(size = 3.0, na.rm = TRUE) +
    facet_grid(dataset ~ query) +
    scale_color_manual(values = mechanism_colors, name = NULL) +
    scale_y_log10(
      breaks = c(0.01, 1, 100, 10000, 1000000),
      labels = c("0.01", "1", "100", "10K", "1M")
    ) +
    labs(x = NULL, y = "median error (%)") +
    theme_bw(base_size = 40, base_family = base_font) +
    theme(
      panel.border = element_rect(linewidth = 1.0),
      panel.grid.major = element_line(linewidth = 1.0),
      panel.grid.minor = element_blank(),
      legend.position = "top",
      legend.title = element_blank(),
      legend.text = element_text(size = 21),
      legend.margin = margin(0, 0, -5, 0),
      legend.box.margin = margin(0, 0, -20, 0),
      axis.text.x = element_text(angle = 35, hjust = 1, size = 12),
      axis.text.y = element_text(size = 18),
      axis.title = element_text(size = 30),
      strip.text = element_text(size = 22, face = "bold"),
      plot.title = element_blank(),
      plot.margin = margin(2, 5, 5, 5)
    )

  png(filename = out_file, width = 5400, height = 2600, res = 350)
  print(p)
  dev.off()
  message("Combined plot saved to: ", out_file)
}

make_plot(summary_df, "TPCH", file.path(output_dir, "tpch_sf30_bound_quality_paper.png"))
make_plot(summary_df, "JCCH", file.path(output_dir, "jcch_sf30_bound_quality_paper.png"))
make_combined_plot(summary_df, file.path(output_dir, "tpch_jcch_sf30_bound_quality_stacked_paper.png"))
