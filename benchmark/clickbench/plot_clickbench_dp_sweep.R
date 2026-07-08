#!/usr/bin/env Rscript
# ClickBench DP bound-sweep plotter.
# Reads pac_clickhouse_benchmark CSV output and summarizes dp_standard vs dp_sass
# over all ClickBench queries without faceting 43 queries into unreadable panels.

user_lib <- Sys.getenv("R_LIBS_USER")
if (user_lib == "") {
  user_lib <- file.path(Sys.getenv("HOME"), "R", "libs")
}
if (!dir.exists(user_lib)) {
  dir.create(user_lib, recursive = TRUE, showWarnings = FALSE)
}
.libPaths(c(user_lib, .libPaths()))

required_packages <- c("ggplot2", "dplyr", "readr", "scales", "stringr", "tidyr", "systemfonts")
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
  library(tidyr)
})

base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) "Linux Libertine" else "serif"
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_clickbench_dp_sweep.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))

expected_cols <- c(
  "query", "mode", "success", "median_error_pct", "recall",
  "bound_multiplier", "run", "time_ms", "total_time_ms"
)
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
  stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}

mechanism_levels <- c("Bounded DP", "DP-SAA average")
mechanism_colors <- c(
  "Bounded DP" = "#d55e00",
  "DP-SAA average" = "#009900"
)
winner_colors <- c(
  "Bounded DP" = "#d55e00",
  "DP-SAA" = "#009900",
  "Tie" = "#bdbdbd",
  "Suppressed" = "#f0f0f0",
  "Unsupported" = "#252525"
)

format_multiplier <- function(x) {
  ifelse(abs(x - round(x)) < 1e-9, as.character(round(x)), as.character(x))
}

plot_data <- raw %>%
  mutate(
    query_id = as.integer(query),
    query = factor(paste0("Q", query_id), levels = paste0("Q", sort(unique(as.integer(query))))),
    run = as.integer(run),
    success = as.logical(success),
    bound_multiplier = as.numeric(bound_multiplier),
    bound_label = format_multiplier(bound_multiplier),
    median_error_pct = as.numeric(median_error_pct),
    recall = as.numeric(recall),
    time_ms = as.numeric(time_ms),
    total_time_ms = as.numeric(total_time_ms),
    mechanism = case_when(
      mode == "dp_standard" ~ "Bounded DP",
      mode == "dp_sass" ~ "DP-SAA average",
      TRUE ~ mode
    )
  ) %>%
  filter(mechanism %in% mechanism_levels, !is.na(bound_multiplier)) %>%
  mutate(
    mechanism = factor(mechanism, levels = mechanism_levels),
    bound_label = factor(bound_label, levels = format_multiplier(sort(unique(bound_multiplier)))),
    released = success & !is.na(recall) & recall > 0 & !is.na(median_error_pct)
  )

if (nrow(plot_data) == 0) {
  stop("No dp_standard/dp_sass rows left after filtering.")
}

summary_by_query <- plot_data %>%
  group_by(query_id, query, bound_multiplier, bound_label, mechanism) %>%
  summarize(
    runs = n(),
    success_runs = sum(success, na.rm = TRUE),
    released_runs = sum(released, na.rm = TRUE),
    mean_recall = ifelse(success_runs > 0, mean(recall[success], na.rm = TRUE), NA_real_),
    median_error_pct = ifelse(released_runs > 0, median(median_error_pct[released], na.rm = TRUE), NA_real_),
    median_total_time_ms = ifelse(success_runs > 0, median(total_time_ms[success], na.rm = TRUE), NA_real_),
    .groups = "drop"
  ) %>%
  mutate(
    plot_error_pct = ifelse(is.na(median_error_pct), NA_real_, pmax(median_error_pct, 0.001)),
    plot_total_time_ms = ifelse(is.na(median_total_time_ms), NA_real_, pmax(median_total_time_ms, 0.001))
  )

distribution_summary <- summary_by_query %>%
  group_by(bound_multiplier, bound_label, mechanism) %>%
  summarize(
    supported_queries = sum(success_runs > 0),
    released_queries = sum(released_runs > 0),
    median_error_pct = median(median_error_pct[released_runs > 0], na.rm = TRUE),
    p25_error_pct = quantile(median_error_pct[released_runs > 0], 0.25, na.rm = TRUE),
    p75_error_pct = quantile(median_error_pct[released_runs > 0], 0.75, na.rm = TRUE),
    median_total_time_ms = median(median_total_time_ms[success_runs > 0], na.rm = TRUE),
    .groups = "drop"
  )

paired <- summary_by_query %>%
  select(query_id, query, bound_multiplier, bound_label, mechanism, success_runs, released_runs,
         mean_recall, median_error_pct) %>%
  tidyr::pivot_wider(
    names_from = mechanism,
    values_from = c(success_runs, released_runs, mean_recall, median_error_pct)
  ) %>%
  mutate(
    both_unsupported = coalesce(success_runs_Bounded.DP, 0) == 0 & coalesce(success_runs_DP.SAA.average, 0) == 0,
    both_suppressed = !both_unsupported &
      coalesce(released_runs_Bounded.DP, 0) == 0 & coalesce(released_runs_DP.SAA.average, 0) == 0,
    standard_recall = coalesce(mean_recall_Bounded.DP, 0),
    sass_recall = coalesce(mean_recall_DP.SAA.average, 0),
    standard_error = coalesce(median_error_pct_Bounded.DP, Inf),
    sass_error = coalesce(median_error_pct_DP.SAA.average, Inf),
    winner = case_when(
      both_unsupported ~ "Unsupported",
      both_suppressed ~ "Suppressed",
      sass_recall > standard_recall ~ "DP-SAA",
      standard_recall > sass_recall ~ "Bounded DP",
      sass_error < standard_error ~ "DP-SAA",
      standard_error < sass_error ~ "Bounded DP",
      TRUE ~ "Tie"
    ),
    winner = factor(winner, levels = names(winner_colors))
  )

summary_csv <- file.path(output_dir, "clickbench_dp_sweep_summary.csv")
winner_csv <- file.path(output_dir, "clickbench_dp_sweep_winners.csv")
readr::write_csv(distribution_summary, summary_csv)
readr::write_csv(paired, winner_csv)
message("Summary CSV saved to: ", summary_csv)
message("Winner CSV saved to: ", winner_csv)

base_theme <- theme_bw(base_size = 34, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 22),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -14, 0),
    axis.text.x = element_text(size = 22),
    axis.text.y = element_text(size = 22),
    axis.title = element_text(size = 30),
    strip.text = element_text(size = 24, face = "bold"),
    plot.title = element_blank(),
    plot.margin = margin(2, 5, 5, 5)
  )

utility_distribution <- ggplot(
  summary_by_query %>% filter(released_runs > 0),
  aes(x = bound_label, y = plot_error_pct, color = mechanism)
) +
  geom_boxplot(
    aes(group = interaction(bound_label, mechanism)),
    position = position_dodge(width = 0.72),
    width = 0.56,
    outlier.size = 1.2,
    linewidth = 0.8,
    na.rm = TRUE
  ) +
  geom_line(
    data = distribution_summary,
    aes(x = bound_label, y = pmax(median_error_pct, 0.001), color = mechanism, group = mechanism),
    position = position_dodge(width = 0.72),
    linewidth = 1.0,
    na.rm = TRUE
  ) +
  geom_point(
    data = distribution_summary,
    aes(x = bound_label, y = pmax(median_error_pct, 0.001), color = mechanism),
    position = position_dodge(width = 0.72),
    size = 2.6,
    na.rm = TRUE
  ) +
  scale_color_manual(values = mechanism_colors, name = NULL) +
  scale_y_log10(
    breaks = c(0.01, 1, 100, 10000, 1000000),
    labels = c("0.01", "1", "100", "10K", "1M")
  ) +
  labs(x = "bound multiplier", y = "median error (%)") +
  base_theme

utility_plot <- file.path(output_dir, "clickbench_dp_sweep_utility_distribution_paper.png")
png(filename = utility_plot, width = 4000, height = 1900, res = 350)
print(utility_distribution)
dev.off()
message("Utility distribution plot saved to: ", utility_plot)

runtime_distribution <- ggplot(
  summary_by_query %>% filter(success_runs > 0),
  aes(x = bound_label, y = plot_total_time_ms, color = mechanism)
) +
  geom_boxplot(
    aes(group = interaction(bound_label, mechanism)),
    position = position_dodge(width = 0.72),
    width = 0.56,
    outlier.size = 1.2,
    linewidth = 0.8,
    na.rm = TRUE
  ) +
  geom_line(
    data = distribution_summary,
    aes(x = bound_label, y = pmax(median_total_time_ms, 0.001), color = mechanism, group = mechanism),
    position = position_dodge(width = 0.72),
    linewidth = 1.0,
    na.rm = TRUE
  ) +
  geom_point(
    data = distribution_summary,
    aes(x = bound_label, y = pmax(median_total_time_ms, 0.001), color = mechanism),
    position = position_dodge(width = 0.72),
    size = 2.6,
    na.rm = TRUE
  ) +
  scale_color_manual(values = mechanism_colors, name = NULL) +
  scale_y_log10(labels = label_number(scale_cut = cut_short_scale())) +
  labs(x = "bound multiplier", y = "total time (ms)") +
  base_theme

runtime_plot <- file.path(output_dir, "clickbench_dp_sweep_runtime_distribution_paper.png")
png(filename = runtime_plot, width = 4000, height = 1900, res = 350)
print(runtime_distribution)
dev.off()
message("Runtime distribution plot saved to: ", runtime_plot)

winner_plot <- ggplot(paired, aes(x = bound_label, y = reorder(query, -query_id), fill = winner)) +
  geom_tile(color = "white", linewidth = 0.25) +
  scale_fill_manual(values = winner_colors, name = NULL, drop = FALSE) +
  labs(x = "bound multiplier", y = NULL) +
  theme_bw(base_size = 30, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 20),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -14, 0),
    axis.text.x = element_text(size = 22),
    axis.text.y = element_text(size = 10),
    axis.title = element_text(size = 28),
    plot.title = element_blank(),
    plot.margin = margin(2, 5, 5, 5)
  )

winner_plot_file <- file.path(output_dir, "clickbench_dp_sweep_winner_heatmap_paper.png")
png(filename = winner_plot_file, width = 2600, height = 3200, res = 350)
print(winner_plot)
dev.off()
message("Winner heatmap saved to: ", winner_plot_file)
