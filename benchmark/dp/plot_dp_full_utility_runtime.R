#!/usr/bin/env Rscript
# Full DP benchmark plotter: utility and runtime for the five supported queries.

script_file <- sub("^--file=", "", grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "dplyr", "readr", "scales", "tidyr", "systemfonts"))

suppressPackageStartupMessages({
  library(ggplot2)
  library(dplyr)
  library(readr)
  library(scales)
  library(tidyr)
})

base_font <- PaperFont()

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_dp_full_utility_runtime.R path/to/results.csv [output_dir]")
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))

expected_cols <- c(
	"dataset", "query", "mode", "release", "success", "time_ms",
	"median_error_pct", "recall", "bound_multiplier", "run"
)
missing_cols <- setdiff(expected_cols, colnames(raw))
if (length(missing_cols) > 0) {
	stop("Missing expected columns in CSV: ", paste(missing_cols, collapse = ", "))
}
if (!("delta" %in% colnames(raw))) {
	raw$delta <- NA_real_
}
if (!("dp_sass_m" %in% colnames(raw))) {
	raw$dp_sass_m <- NA_integer_
}

scenario_levels <- c("terrible tight", "bad tight", "good", "bad loose", "terrible loose")
query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
mechanism_levels <- c(
  "DuckDB", "Bounded DP", "DP-Elastic", "DP-SAA median", "DP-SAA average"
)
mechanism_colors <- c(
  "DuckDB" = "#95a5a6",
  "Bounded DP" = "#d55e00",
  "DP-Elastic" = "#0072b2",
  "DP-SAA median" = "#4dff4d",
  "DP-SAA average" = "#009900"
)
utility_mechanism_colors <- mechanism_colors[names(mechanism_colors) != "DuckDB"]
utility_mechanism_shapes <- c(
  "Bounded DP" = 16,
  "DP-Elastic" = 17,
  "DP-SAA median" = 15,
  "DP-SAA average" = 18
)
runtime_mechanism_levels <- c("DuckDB", "Bounded DP", "DP-Elastic", "DP-SAA m=64", "DP-SAA m=512")
runtime_mechanism_colors <- c(
	"DuckDB" = "#95a5a6",
	"Bounded DP" = "#d55e00",
	"DP-Elastic" = "#0072b2",
	"DP-SAA m=64" = "#4dff4d",
	"DP-SAA m=512" = "#009900"
)
runtime_query_spacing <- 0.82

normalized <- raw %>%
  mutate(
    dataset = case_when(
      tolower(dataset) == "tpch" ~ "TPC-H",
      TRUE ~ toupper(dataset)
    ),
    query = toupper(query),
		run = as.integer(run),
		bound_multiplier = as.numeric(bound_multiplier),
		delta = as.numeric(delta),
		dp_sass_m = as.integer(dp_sass_m),
		time_ms = as.numeric(time_ms),
    median_error_pct = as.numeric(median_error_pct),
    recall = as.numeric(recall),
    mechanism = case_when(
      mode == "duckdb" ~ "DuckDB",
      mode == "dp_standard" ~ "Bounded DP",
      mode == "dp_elastic" ~ "DP-Elastic",
      mode == "dp_sass" & release == "median" ~ "DP-SAA median",
      mode == "dp_sass" & release == "average" ~ "DP-SAA average",
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
    usable = success & !is.na(time_ms) & time_ms > 0
  ) %>%
  filter(mechanism %in% mechanism_levels, query %in% query_levels) %>%
  mutate(
    mechanism = factor(mechanism, levels = mechanism_levels),
    query = factor(query, levels = query_levels),
    scenario = factor(scenario, levels = scenario_levels)
  )

if (nrow(normalized) == 0) {
  stop("No benchmark rows left after filtering.")
}

duckdb_baseline <- normalized %>%
  filter(mechanism == "DuckDB", usable) %>%
  group_by(dataset, query) %>%
  summarize(duckdb_time_ms = median(time_ms, na.rm = TRUE), .groups = "drop")

if (nrow(duckdb_baseline) == 0) {
  stop("No successful DuckDB baseline rows found; cannot compute slowdown.")
}

utility_summary <- normalized %>%
  filter(mechanism != "DuckDB", scenario %in% scenario_levels) %>%
  mutate(utility_usable = usable & !is.na(recall) & recall > 0 & !is.na(median_error_pct)) %>%
  group_by(dataset, query, mechanism, scenario) %>%
  summarize(
    runs = n(),
    usable_runs = sum(utility_usable),
    median_error_pct = ifelse(
      usable_runs > 0,
      median(median_error_pct[utility_usable], na.rm = TRUE),
      NA_real_
    ),
    p25_error_pct = ifelse(
      usable_runs > 0,
      quantile(median_error_pct[utility_usable], 0.25, na.rm = TRUE),
      NA_real_
    ),
    p75_error_pct = ifelse(
      usable_runs > 0,
      quantile(median_error_pct[utility_usable], 0.75, na.rm = TRUE),
      NA_real_
    ),
    .groups = "drop"
  ) %>%
  mutate(plot_value = ifelse(is.na(median_error_pct), NA_real_, pmax(median_error_pct, 0.001)))

runtime_summary_mechanisms <- normalized %>%
  filter(mechanism != "DuckDB", usable, scenario %in% scenario_levels) %>%
  group_by(dataset, query, mechanism, scenario) %>%
  summarize(
    runs = n(),
    median_time_ms = median(time_ms, na.rm = TRUE),
    .groups = "drop"
  )

runtime_duckdb <- duckdb_baseline %>%
  tidyr::crossing(scenario = factor(scenario_levels, levels = scenario_levels)) %>%
  mutate(
    mechanism = factor("DuckDB", levels = mechanism_levels),
    median_time_ms = duckdb_time_ms,
    runs = NA_integer_
  ) %>%
  select(dataset, query, mechanism, scenario, runs, median_time_ms)

runtime_summary <- bind_rows(runtime_summary_mechanisms, runtime_duckdb) %>%
  left_join(duckdb_baseline, by = c("dataset", "query")) %>%
  mutate(
    slowdown = median_time_ms / duckdb_time_ms,
    plot_value = pmax(slowdown, 0.001)
  )

utility_summary_grouped <- utility_summary
runtime_summary_grouped <- runtime_summary

runtime_delta_candidates <- normalized %>%
	filter(
		dataset == "TPC-H",
		usable,
		abs(bound_multiplier - 1.0) <= 1e-12,
		mode %in% c("dp_standard", "dp_elastic", "dp_sass"),
		!is.na(delta)
	) %>%
	pull(delta) %>%
	unique() %>%
	sort()
runtime_delta <- if (length(runtime_delta_candidates) == 0) {
	NA_real_
} else if (any(abs(runtime_delta_candidates - 1e-6) <= 1e-12)) {
	1e-6
} else {
	runtime_delta_candidates[1]
}
runtime_compact <- normalized %>%
	filter(dataset == "TPC-H", usable) %>%
	filter(
		mode == "duckdb" |
			(
				abs(bound_multiplier - 1.0) <= 1e-12 &
					(is.na(runtime_delta) | is.na(delta) |
						abs(delta - runtime_delta) <= max(1e-12, runtime_delta * 1e-8)) &
					(
						mode %in% c("dp_standard", "dp_elastic") |
							(mode == "dp_sass" & release == "average" & dp_sass_m %in% c(64L, 512L))
					)
			)
	) %>%
	mutate(
		mechanism = case_when(
			mode == "duckdb" ~ "DuckDB",
			mode == "dp_standard" ~ "Bounded DP",
			mode == "dp_elastic" ~ "DP-Elastic",
			mode == "dp_sass" & release == "average" & dp_sass_m == 64L ~ "DP-SAA m=64",
			mode == "dp_sass" & release == "average" & dp_sass_m == 512L ~ "DP-SAA m=512",
			TRUE ~ NA_character_
		),
		mechanism = factor(mechanism, levels = runtime_mechanism_levels)
	) %>%
	filter(mechanism %in% runtime_mechanism_levels) %>%
	left_join(duckdb_baseline, by = c("dataset", "query")) %>%
	mutate(slowdown = time_ms / duckdb_time_ms) %>%
	group_by(query, mechanism) %>%
	summarize(
		avg_time_ms = mean(time_ms, na.rm = TRUE),
		median_time_ms = median(time_ms, na.rm = TRUE),
		avg_slowdown = mean(slowdown, na.rm = TRUE),
		median_slowdown = median(slowdown, na.rm = TRUE),
		min_slowdown = min(slowdown, na.rm = TRUE),
    max_slowdown = max(slowdown, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(
    query = factor(query, levels = query_levels),
    mechanism = factor(mechanism, levels = runtime_mechanism_levels),
		qidx = match(as.character(query), query_levels),
		x_base = qidx * runtime_query_spacing,
		x_pos = x_base + case_when(
			mechanism == "DuckDB" ~ -0.28,
			mechanism == "Bounded DP" ~ -0.14,
			mechanism == "DP-Elastic" ~ 0.0,
			mechanism == "DP-SAA m=64" ~ 0.14,
			mechanism == "DP-SAA m=512" ~ 0.28,
			TRUE ~ 0.0
		),
    label_y = avg_time_ms * 1.15,
    slowdown_label = ifelse(mechanism == "DuckDB", "",
                            ifelse(avg_slowdown >= 10, sprintf("%.0fx", avg_slowdown),
                                   sprintf("%.1fx", avg_slowdown)))
  )

utility_csv <- file.path(output_dir, "tpch_sf30_all5_utility_summary.csv")
runtime_csv <- file.path(output_dir, "tpch_sf30_all5_runtime_summary.csv")
runtime_compact_csv <- file.path(output_dir, "tpch_sf30_all5_runtime_compact_summary.csv")
readr::write_csv(utility_summary, utility_csv)
readr::write_csv(runtime_summary, runtime_csv)
readr::write_csv(runtime_compact, runtime_compact_csv)
message("Utility summary saved to: ", utility_csv)
message("Runtime summary saved to: ", runtime_csv)
message("Compact runtime summary saved to: ", runtime_compact_csv)

base_theme <- theme_bw(base_size = 34, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 18),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -14, 0),
    axis.text.x = element_text(angle = 35, hjust = 1, size = 12),
    axis.text.y = element_text(size = 16),
    axis.title = element_text(size = 24),
    strip.text = element_text(size = 18, face = "bold"),
    plot.title = element_blank(),
    plot.margin = margin(2, 5, 5, 5)
  )

make_metric_plot <- function(df, y_label, out_file, breaks, labels) {
  p <- ggplot(df, aes(x = scenario, y = plot_value, color = mechanism, group = mechanism)) +
    geom_line(linewidth = 1.0, na.rm = TRUE) +
    geom_point(size = 2.4, na.rm = TRUE) +
    facet_grid(dataset ~ query, scales = "free_y") +
    scale_color_manual(values = mechanism_colors, name = NULL, drop = FALSE) +
    scale_y_log10(breaks = breaks, labels = labels) +
    labs(x = NULL, y = y_label) +
    base_theme

  png(filename = out_file, width = 5600, height = 2600, res = 350)
  print(p)
  dev.off()
  message("Plot saved to: ", out_file)
}

utility_plot <- file.path(output_dir, "tpch_sf30_all5_utility_paper.png")
runtime_plot <- file.path(output_dir, "tpch_sf30_all5_runtime_paper.png")
utility_grouped_plot <- file.path(output_dir, "tpch_sf30_all5_utility_grouped_paper.png")
runtime_compact_plot <- file.path(output_dir, "tpch_sf30_all5_runtime_compact_paper.png")
combined_plot <- file.path(output_dir, "tpch_sf30_all5_utility_runtime_paper.png")
combined_grouped_plot <- file.path(output_dir, "tpch_sf30_all5_utility_runtime_grouped_paper.png")

make_metric_plot(
  utility_summary,
  "median error (%)",
  utility_plot,
  c(0.01, 1, 100, 10000, 1000000),
  c("0.01", "1", "100", "10K", "1M")
)
make_metric_plot(
  runtime_summary,
  "slowdown vs DuckDB (x)",
  runtime_plot,
  c(1, 2, 5, 10, 50, 100),
  c("1", "2", "5", "10", "50", "100")
)

utility_grouped <- ggplot(
  utility_summary_grouped,
  aes(x = scenario, y = plot_value, color = mechanism, shape = mechanism, group = mechanism)
) +
  geom_line(linewidth = 1.15, na.rm = TRUE) +
  geom_point(size = 3.2, na.rm = TRUE) +
  facet_grid(dataset ~ query, scales = "free_y") +
  scale_color_manual(values = utility_mechanism_colors, name = NULL) +
  scale_shape_manual(values = utility_mechanism_shapes, name = NULL) +
  scale_y_log10(
    breaks = c(0.01, 1, 100, 10000, 1000000),
    labels = c("0.01", "1", "100", "10K", "1M")
  ) +
  labs(x = NULL, y = "median error (%)") +
  theme_bw(base_size = 48, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 35),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -14, 0),
    axis.text.x = element_text(angle = 35, hjust = 1, size = 23),
    axis.text.y = element_text(size = 23),
    axis.title = element_text(size = 33),
    strip.text = element_text(size = 23, face = "bold", margin = margin(5, 0, 5, 0)),
    strip.background = element_rect(fill = "grey85", color = "grey20", linewidth = 1.0),
    plot.margin = margin(2, 5, 5, 5)
  )

png(filename = utility_grouped_plot, width = 11200, height = 4000, res = 700)
print(utility_grouped)
dev.off()
message("Grouped utility plot saved to: ", utility_grouped_plot)

runtime_compact_plot_obj <- ggplot(
  runtime_compact,
  aes(x = x_pos, y = avg_time_ms, fill = mechanism)
) +
  geom_col(width = 0.14, color = "black", linewidth = 0.25, na.rm = TRUE) +
	geom_text(
		data = runtime_compact %>% filter(mechanism != "DuckDB"),
		aes(x = x_pos, y = label_y, label = slowdown_label),
		inherit.aes = FALSE,
		size = 4.5,
    vjust = 0,
    fontface = "bold",
    family = base_font,
    na.rm = TRUE
  ) +
  scale_fill_manual(values = runtime_mechanism_colors, name = NULL, drop = FALSE) +
  scale_x_continuous(
    breaks = seq_along(query_levels) * runtime_query_spacing,
    labels = query_levels,
    expand = expansion(add = 0.08)
  ) +
  scale_y_log10(labels = function(x) ifelse(x >= 1000, paste0(x / 1000, "s"), paste0(x, "ms"))) +
  coord_cartesian(ylim = c(NA, max(runtime_compact$avg_time_ms, na.rm = TRUE) * 1.5), clip = "off") +
  guides(fill = guide_legend(nrow = 1, byrow = TRUE)) +
  labs(x = NULL, y = NULL) +
  theme_bw(base_size = 40, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 1.0),
    panel.grid.minor = element_blank(),
		legend.position = "top",
		legend.justification = "right",
		legend.box.just = "right",
		legend.direction = "horizontal",
		legend.title = element_blank(),
		legend.text = element_text(size = 22),
		legend.key.size = unit(0.5, "cm"),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -20, 0),
    axis.text.x = element_text(angle = 0, hjust = 0.5, size = 24),
    axis.text.y = element_text(size = 24),
    axis.title = element_text(size = 32),
    plot.title = element_blank(),
    plot.margin = margin(2, 5, 5, 5)
  )

png(filename = runtime_compact_plot, width = 4000, height = 1300, res = 350)
print(runtime_compact_plot_obj)
dev.off()
message("Compact runtime plot saved to: ", runtime_compact_plot)

combined_data <- bind_rows(
  utility_summary %>%
    mutate(metric = "median error (%)") %>%
    select(dataset, query, mechanism, scenario, plot_value, metric),
  runtime_summary %>%
    mutate(metric = "slowdown vs DuckDB (x)") %>%
    select(dataset, query, mechanism, scenario, plot_value, metric)
) %>%
  mutate(metric = factor(metric, levels = c("median error (%)", "slowdown vs DuckDB (x)")))

combined <- ggplot(combined_data, aes(x = scenario, y = plot_value, color = mechanism, group = mechanism)) +
  geom_line(linewidth = 0.9, na.rm = TRUE) +
  geom_point(size = 2.1, na.rm = TRUE) +
  facet_grid(metric + dataset ~ query, scales = "free_y") +
  scale_color_manual(values = mechanism_colors, name = NULL, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.01, 0.1, 1, 10, 100, 10000, 1000000),
    labels = c("0.01", "0.1", "1", "10", "100", "10K", "1M")
  ) +
  labs(x = NULL, y = NULL) +
  base_theme +
  theme(
    axis.text.x = element_text(angle = 35, hjust = 1, size = 9),
    axis.text.y = element_text(size = 12),
    strip.text = element_text(size = 14, face = "bold")
  )

png(filename = combined_plot, width = 5800, height = 4300, res = 350)
print(combined)
dev.off()
message("Combined plot saved to: ", combined_plot)

combined_grouped_data <- bind_rows(
  utility_summary %>%
    mutate(metric = "median error (%)") %>%
    select(dataset, query, mechanism, scenario, plot_value, metric),
  runtime_summary %>%
    mutate(metric = "slowdown vs DuckDB (x)") %>%
    select(dataset, query, mechanism, scenario, plot_value, metric)
) %>%
  mutate(
    metric = factor(metric, levels = c("median error (%)", "slowdown vs DuckDB (x)")),
    dataset_query = paste(dataset, query, sep = " ")
  )

short_theme <- theme_bw(base_size = 42, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 0.8),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 22),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -12, 0),
    axis.text.x = element_text(angle = 35, hjust = 1, size = 11),
    axis.text.y = element_text(size = 15),
    strip.text = element_text(size = 16, face = "bold"),
    plot.margin = margin(2, 5, 5, 5)
  )

combined_grouped <- ggplot(
  combined_grouped_data,
  aes(x = scenario, y = plot_value, color = mechanism, group = mechanism)
) +
  geom_line(linewidth = 0.95, na.rm = TRUE) +
  geom_point(size = 2.1, na.rm = TRUE) +
  facet_grid(metric ~ dataset_query, scales = "free_y") +
  scale_color_manual(values = mechanism_colors, name = NULL, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.01, 0.1, 1, 10, 100, 10000, 1000000),
    labels = c("0.01", "0.1", "1", "10", "100", "10K", "1M")
  ) +
  labs(x = NULL, y = NULL) +
  short_theme

png(filename = combined_grouped_plot, width = 7000, height = 2500, res = 350)
print(combined_grouped)
dev.off()
message("Grouped combined plot saved to: ", combined_grouped_plot)
