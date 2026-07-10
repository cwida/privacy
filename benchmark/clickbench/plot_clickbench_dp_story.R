#!/usr/bin/env Rscript
# Paper-style ClickBench DP utility/performance plotter.
# Reads pac_clickhouse_benchmark CSV output and writes separate summaries for
# utility, recall/precision, per-query slowdown against DuckDB, and winner counts.

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
  stop("Usage: Rscript plot_clickbench_dp_story.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

as_bool <- function(x) {
  if (is.logical(x)) {
    return(x)
  }
  tolower(as.character(x)) %in% c("true", "t", "1", "yes")
}

safe_mean <- function(x) {
  x <- x[is.finite(x)]
  if (length(x) == 0) NA_real_ else mean(x)
}

safe_median <- function(x) {
  x <- x[is.finite(x)]
  if (length(x) == 0) NA_real_ else median(x)
}

safe_quantile <- function(x, p) {
  x <- x[is.finite(x)]
  if (length(x) == 0) NA_real_ else unname(quantile(x, p, na.rm = TRUE))
}

format_multiplier <- function(x) {
  ifelse(abs(x - round(x)) < 1e-9, as.character(round(x)), as.character(x))
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))

if (!("sass_release" %in% names(raw))) {
  raw$sass_release <- ""
}
if (!("common_dp_query" %in% names(raw))) {
  raw$common_dp_query <- TRUE
} else {
  raw$common_dp_query <- as_bool(raw$common_dp_query)
}
if (!("query_category" %in% names(raw))) {
  raw$query_category <- ""
}

expected_cols <- c(
  "query", "mode", "success", "median_error_pct", "recall", "precision",
  "bound_multiplier", "run", "time_ms", "total_time_ms"
)
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
  stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}

if (any(raw$common_dp_query, na.rm = TRUE)) {
  raw <- raw %>% filter(common_dp_query)
  message("Filtered to common DP ClickBench query set.")
}

mechanism_levels <- c("DP standard", "DP elastic", "SASS median", "SASS average")
mechanism_colors <- c(
  "DP standard" = "#95a5a6",
  "DP elastic" = "#a8d4ff",
  "SASS median" = "#4dff4d",
  "SASS average" = "#009900"
)
winner_colors <- c(
  "DP standard" = "#95a5a6",
  "DP elastic" = "#a8d4ff",
  "SASS median" = "#4dff4d",
  "SASS average" = "#009900",
  "Tie" = "#bdbdbd",
  "Suppressed" = "#f0f0f0",
  "Failed" = "#fb9a99",
  "Missing" = "#252525"
)

baseline <- raw %>%
  mutate(
    query_id = as.integer(query),
    success = as_bool(success),
    total_time_ms = as.numeric(total_time_ms),
    time_ms = as.numeric(time_ms)
  ) %>%
  filter(mode == "baseline", success) %>%
  group_by(query_id) %>%
  summarize(
    baseline_mean_ms = safe_mean(ifelse(is.finite(total_time_ms), total_time_ms, time_ms)),
    .groups = "drop"
  )

if (nrow(baseline) == 0) {
  stop("No successful DuckDB baseline rows found; cannot compute slowdown.")
}

slowdown_col <- if ("slowdown_vs_duckdb" %in% names(raw)) "slowdown_vs_duckdb" else NA_character_

plot_data <- raw %>%
  mutate(
    query_id = as.integer(query),
    query = factor(paste0("Q", query_id), levels = paste0("Q", sort(unique(as.integer(query))))),
    run = as.integer(run),
    success = as_bool(success),
    bound_multiplier = as.numeric(bound_multiplier),
    bound_label = format_multiplier(bound_multiplier),
    median_error_pct = as.numeric(median_error_pct),
    recall = as.numeric(recall),
    precision = as.numeric(precision),
    time_ms = as.numeric(time_ms),
    total_time_ms = as.numeric(total_time_ms),
    mechanism = case_when(
      mode == "dp_standard" ~ "DP standard",
      mode == "dp_elastic" ~ "DP elastic",
      mode == "dp_sass" & sass_release == "median" ~ "SASS median",
      mode == "dp_sass" & sass_release == "average" ~ "SASS average",
      TRUE ~ mode
    ),
    csv_slowdown = if (!is.na(slowdown_col)) as.numeric(.data[[slowdown_col]]) else NA_real_
  ) %>%
  filter(mechanism %in% mechanism_levels, !is.na(bound_multiplier)) %>%
  left_join(baseline, by = "query_id") %>%
  mutate(
    mechanism = factor(mechanism, levels = mechanism_levels),
    bound_label = factor(bound_label, levels = format_multiplier(sort(unique(bound_multiplier)))),
    computed_slowdown = ifelse(baseline_mean_ms > 0, total_time_ms / baseline_mean_ms, NA_real_),
    slowdown_vs_duckdb = coalesce(csv_slowdown, computed_slowdown),
    released = success & !is.na(recall) & recall > 0 & !is.na(median_error_pct)
  )

if (nrow(plot_data) == 0) {
  stop("No DP rows left after filtering.")
}

query_summary <- plot_data %>%
  group_by(query_id, query, query_category, bound_multiplier, bound_label, mechanism) %>%
  summarize(
    runs = n(),
    success_runs = sum(success, na.rm = TRUE),
    released_runs = sum(released, na.rm = TRUE),
    mean_recall = safe_mean(recall[success]),
    median_recall = safe_median(recall[success]),
    mean_precision = safe_mean(precision[success]),
    median_precision = safe_median(precision[success]),
    median_error_pct = safe_median(median_error_pct[released]),
    mean_error_pct = safe_mean(median_error_pct[released]),
    mean_slowdown_vs_duckdb = safe_mean(slowdown_vs_duckdb[success]),
    median_slowdown_vs_duckdb = safe_median(slowdown_vs_duckdb[success]),
    duckdb_baseline_ms = first(baseline_mean_ms),
    .groups = "drop"
  )

line_summary <- query_summary %>%
  group_by(bound_multiplier, bound_label, mechanism) %>%
  summarize(
    query_count = n(),
    supported_queries = sum(success_runs > 0),
    released_queries = sum(released_runs > 0),
    median_error_pct = safe_median(median_error_pct[released_runs > 0]),
    mean_error_pct = safe_mean(median_error_pct[released_runs > 0]),
    p25_error_pct = safe_quantile(median_error_pct[released_runs > 0], 0.25),
    p75_error_pct = safe_quantile(median_error_pct[released_runs > 0], 0.75),
    mean_recall = safe_mean(mean_recall[success_runs > 0]),
    p25_recall = safe_quantile(mean_recall[success_runs > 0], 0.25),
    p75_recall = safe_quantile(mean_recall[success_runs > 0], 0.75),
    mean_precision = safe_mean(mean_precision[success_runs > 0]),
    mean_slowdown_vs_duckdb = safe_mean(mean_slowdown_vs_duckdb[success_runs > 0]),
    .groups = "drop"
  )

make_winner_rows <- function(df, comparison_name, mechanisms) {
  scoped <- df %>% filter(as.character(mechanism) %in% mechanisms)
  status <- scoped %>%
    group_by(query_id, query, bound_multiplier, bound_label) %>%
    summarize(
      mechanisms_present = n_distinct(as.character(mechanism)),
      success_mechanisms = sum(success_runs > 0),
      released_mechanisms = sum(released_runs > 0),
      .groups = "drop"
    )

  candidates <- scoped %>%
    filter(released_runs > 0, is.finite(mean_recall), is.finite(median_error_pct)) %>%
    group_by(query_id, query, bound_multiplier, bound_label) %>%
    mutate(best_recall = max(mean_recall, na.rm = TRUE)) %>%
    filter(abs(mean_recall - best_recall) < 1e-12) %>%
    mutate(best_error = min(median_error_pct, na.rm = TRUE)) %>%
    filter(abs(median_error_pct - best_error) < 1e-12) %>%
    summarize(
      candidate = ifelse(n_distinct(as.character(mechanism)) > 1, "Tie", as.character(first(mechanism))),
      .groups = "drop"
    )

  status %>%
    left_join(candidates, by = c("query_id", "query", "bound_multiplier", "bound_label")) %>%
    mutate(
      comparison = comparison_name,
      winner = case_when(
        mechanisms_present < length(mechanisms) ~ "Missing",
        success_mechanisms < length(mechanisms) ~ "Failed",
        released_mechanisms == 0 ~ "Suppressed",
        !is.na(candidate) ~ candidate,
        TRUE ~ "Tie"
      )
    )
}

winner_rows <- bind_rows(
  make_winner_rows(query_summary, "all DP mechanisms", mechanism_levels),
  make_winner_rows(query_summary, "user-level mechanisms", c("DP standard", "SASS median", "SASS average"))
) %>%
  mutate(
    winner = factor(winner, levels = names(winner_colors)),
    comparison = factor(comparison, levels = c("all DP mechanisms", "user-level mechanisms"))
  )

winner_counts <- winner_rows %>%
  count(comparison, bound_multiplier, bound_label, winner, name = "queries")

perf_multiplier <- if (any(abs(plot_data$bound_multiplier - 1.0) < 1e-12, na.rm = TRUE)) {
  1.0
} else {
  sort(unique(plot_data$bound_multiplier))[1]
}

runtime_by_query <- query_summary %>%
  filter(abs(bound_multiplier - perf_multiplier) < 1e-12, success_runs > 0) %>%
  group_by(query_id, query, query_category, mechanism) %>%
  summarize(
    duckdb_baseline_ms = first(duckdb_baseline_ms),
    mean_slowdown_vs_duckdb = safe_mean(mean_slowdown_vs_duckdb),
    median_slowdown_vs_duckdb = safe_median(median_slowdown_vs_duckdb),
    .groups = "drop"
  )

summary_csv <- file.path(output_dir, "clickbench_dp_story_line_summary.csv")
query_csv <- file.path(output_dir, "clickbench_dp_story_query_summary.csv")
winner_csv <- file.path(output_dir, "clickbench_dp_story_winners.csv")
runtime_csv <- file.path(output_dir, "clickbench_dp_story_runtime_by_query.csv")
readr::write_csv(line_summary, summary_csv)
readr::write_csv(query_summary, query_csv)
readr::write_csv(winner_rows, winner_csv)
readr::write_csv(runtime_by_query, runtime_csv)
message("Line summary CSV saved to: ", summary_csv)
message("Query summary CSV saved to: ", query_csv)
message("Winner CSV saved to: ", winner_csv)
message("Runtime CSV saved to: ", runtime_csv)

base_theme <- theme_bw(base_size = 34, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 21),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -14, 0),
    axis.text.x = element_text(size = 20),
    axis.text.y = element_text(size = 20),
    axis.title = element_text(size = 28),
    strip.text = element_text(size = 23, face = "bold"),
    plot.title = element_blank(),
    plot.margin = margin(2, 5, 5, 5)
  )

utility_plot <- ggplot(
  line_summary,
  aes(x = bound_label, y = pmax(median_error_pct, 0.001), color = mechanism, group = mechanism)
) +
  geom_line(linewidth = 1.2, na.rm = TRUE) +
  geom_point(size = 3.1, na.rm = TRUE) +
  geom_errorbar(
    aes(ymin = pmax(p25_error_pct, 0.001), ymax = pmax(p75_error_pct, 0.001)),
    width = 0.08,
    linewidth = 0.8,
    na.rm = TRUE
  ) +
  scale_color_manual(values = mechanism_colors, name = NULL, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.01, 1, 100, 10000, 1000000),
    labels = c("0.01", "1", "100", "10K", "1M")
  ) +
  labs(x = "bound multiplier", y = "median error (%)") +
  base_theme

utility_file <- file.path(output_dir, "clickbench_dp_story_error_line_paper.png")
png(filename = utility_file, width = 4200, height = 1700, res = 350)
print(utility_plot)
dev.off()
message("Utility line plot saved to: ", utility_file)

recall_plot <- ggplot(
  line_summary,
  aes(x = bound_label, y = mean_recall, color = mechanism, group = mechanism)
) +
  geom_line(linewidth = 1.2, na.rm = TRUE) +
  geom_point(size = 3.1, na.rm = TRUE) +
  geom_errorbar(
    aes(ymin = pmax(p25_recall, 0), ymax = pmin(p75_recall, 1)),
    width = 0.08,
    linewidth = 0.8,
    na.rm = TRUE
  ) +
  scale_color_manual(values = mechanism_colors, name = NULL, drop = FALSE) +
  scale_y_continuous(labels = percent_format(accuracy = 1), limits = c(0, 1)) +
  labs(x = "bound multiplier", y = "mean recall") +
  base_theme

recall_file <- file.path(output_dir, "clickbench_dp_story_recall_line_paper.png")
png(filename = recall_file, width = 4200, height = 1700, res = 350)
print(recall_plot)
dev.off()
message("Recall line plot saved to: ", recall_file)

winner_plot <- ggplot(winner_counts, aes(x = bound_label, y = queries, fill = winner)) +
  geom_col(width = 0.72, color = "white", linewidth = 0.35) +
  facet_wrap(~ comparison, nrow = 1) +
  scale_fill_manual(values = winner_colors, name = NULL, drop = FALSE) +
  labs(x = "bound multiplier", y = "queries") +
  base_theme +
  theme(axis.text.x = element_text(size = 18))

winner_file <- file.path(output_dir, "clickbench_dp_story_winner_stacked_paper.png")
png(filename = winner_file, width = 4200, height = 1700, res = 350)
print(winner_plot)
dev.off()
message("Winner stacked plot saved to: ", winner_file)

runtime_plot <- ggplot(
  runtime_by_query,
  aes(x = reorder(query, query_id), y = pmax(mean_slowdown_vs_duckdb, 0.05), color = mechanism, group = mechanism)
) +
  geom_hline(yintercept = 1, linewidth = 0.7, color = "#666666") +
  geom_line(linewidth = 1.0, na.rm = TRUE) +
  geom_point(size = 2.4, na.rm = TRUE) +
  scale_color_manual(values = mechanism_colors, name = NULL, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256),
    labels = c("0.25x", "0.5x", "1x", "2x", "4x", "8x", "16x", "32x", "64x", "128x", "256x")
  ) +
  labs(x = NULL, y = "mean slowdown vs. DuckDB") +
  base_theme +
  theme(axis.text.x = element_text(size = 14))

runtime_file <- file.path(output_dir, "clickbench_dp_story_runtime_per_query_paper.png")
png(filename = runtime_file, width = 5000, height = 1700, res = 350)
print(runtime_plot)
dev.off()
message("Runtime per-query plot saved to: ", runtime_file)
