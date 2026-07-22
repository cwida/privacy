#!/usr/bin/env Rscript
# Paper-style utility views for the unskewed DP/SAA run.

script_file <- sub("^--file=", "", grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "dplyr", "readr", "scales", "systemfonts"))

suppressPackageStartupMessages({
  library(ggplot2)
  library(dplyr)
  library(readr)
  library(scales)
})

base_font <- PaperFont()

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop(
    "Usage: Rscript plot_unskewed_sass_utility_views.R path/to/results.csv [output_dir] ",
    "[delta=1e-6] [sass_m=64] [sass_rescale=false]"
  )
}

input_csv <- args[1]
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
target_delta <- if (length(args) >= 3) as.numeric(args[3]) else 1e-6
target_m <- if (length(args) >= 4) as.integer(args[4]) else 64L
target_rescale <- if (length(args) >= 5) tolower(args[5]) else "false"

if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
expected_cols <- c(
	"dataset", "query", "mode", "release", "success", "run", "bound_multiplier",
	"median_error_pct", "saa_estimator_median_error_pct"
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
if (!("dp_sass_rescale" %in% colnames(raw))) {
	raw$dp_sass_rescale <- NA_character_
}
if (!("saa_noise_scale_median" %in% colnames(raw))) {
	raw$saa_noise_scale_median <- NA_real_
}
if (!("target_median_error_pct" %in% colnames(raw))) {
	raw$target_median_error_pct <- NA_real_
}

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
mechanism_levels <- c("Bounded DP", "DP-Elastic", "SAA", "SAA median")
mechanism_colors <- c(
  "Bounded DP" = "#d55e00",
  "DP-Elastic" = "#0072b2",
  "SAA" = "#009900",
  "SAA median" = "#4dff4d"
)
mechanism_shapes <- c(
  "Bounded DP" = 16,
  "DP-Elastic" = 17,
  "SAA" = 15,
  "SAA median" = 18
)
metric_levels <- c("Full error (%)", "Target error (%)", "Noise scale")

delta_matches <- function(values, target) {
  if (all(is.na(values))) {
    return(rep(TRUE, length(values)))
  }
  abs(values - target) <= max(1e-15, abs(target) * 1e-8)
}

bound_label <- function(x) {
  case_when(
    abs(x - 0.01) < 1e-12 ~ "0.01x",
    abs(x - 0.1) < 1e-12 ~ "0.1x",
    abs(x - 1.0) < 1e-12 ~ "1x",
    abs(x - 10.0) < 1e-12 ~ "10x",
    abs(x - 100.0) < 1e-12 ~ "100x",
    abs(x - 1000.0) < 1e-12 ~ "1000x",
    TRUE ~ paste0(format(x, trim = TRUE, scientific = FALSE), "x")
  )
}

normalized <- raw %>%
	mutate(
    dataset = toupper(dataset),
    query = toupper(query),
    success = SuccessFlag(success),
    run = suppressWarnings(as.integer(run)),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    delta = suppressWarnings(as.numeric(delta)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    dp_sass_rescale = tolower(as.character(dp_sass_rescale)),
	full_error = suppressWarnings(as.numeric(median_error_pct)),
	estimator_error = suppressWarnings(as.numeric(saa_estimator_median_error_pct)),
	generic_target_error = suppressWarnings(as.numeric(target_median_error_pct)),
	saa_noise_scale = suppressWarnings(as.numeric(saa_noise_scale_median)),
	mechanism = case_when(
      mode == "dp_standard" ~ "Bounded DP",
      mode == "dp_elastic" ~ "DP-Elastic",
      mode == "dp_sass" & release == "average" ~ "SAA",
      mode == "dp_sass" & release == "median" ~ "SAA median",
      TRUE ~ NA_character_
    )
  ) %>%
  filter(dataset == "TPCH", success, query %in% query_levels, !is.na(mechanism), !is.na(bound_multiplier)) %>%
  filter(delta_matches(delta, target_delta)) %>%
  filter(
    !(mechanism %in% c("SAA", "SAA median")) |
      (dp_sass_m == target_m & (is.na(dp_sass_rescale) | dp_sass_rescale == target_rescale))
  ) %>%
  mutate(
    mechanism = factor(mechanism, levels = mechanism_levels[mechanism_levels %in% unique(mechanism)]),
    query = factor(query, levels = query_levels)
  )

if (nrow(normalized) == 0) {
  stop("No usable TPCH rows left after filtering.")
}

metric_rows <- bind_rows(
	normalized %>%
		transmute(
			query, mechanism, run, bound_multiplier,
			metric = "Full error (%)", value = full_error
		),
	normalized %>%
		transmute(
			query, mechanism, run, bound_multiplier,
			metric = "Target error (%)",
			value = case_when(
				!is.na(generic_target_error) ~ generic_target_error,
				mechanism %in% c("SAA", "SAA median") ~ estimator_error,
				TRUE ~ full_error
			)
		),
	normalized %>%
		filter(mechanism %in% c("SAA", "SAA median")) %>%
		transmute(
			query, mechanism, run, bound_multiplier,
			metric = "Noise scale", value = saa_noise_scale
		)
) %>%
  filter(!is.na(value), is.finite(value)) %>%
  mutate(metric = factor(metric, levels = metric_levels))

summary <- metric_rows %>%
  group_by(query, mechanism, metric, bound_multiplier) %>%
  summarize(
    value = median(value, na.rm = TRUE),
    runs = n(),
    .groups = "drop"
  ) %>%
  mutate(
    bound = bound_multiplier,
    bound_label = bound_label(bound_multiplier),
    plot_value = pmax(value, 0.001)
  )

if (nrow(summary) == 0) {
  stop("No metric rows left after filtering.")
}

suffix <- paste0(
  "delta", format(target_delta, scientific = TRUE, trim = TRUE),
  "_m", target_m,
  "_rescale", target_rescale
)
suffix <- gsub("[+]", "", suffix)
suffix <- gsub("[^A-Za-z0-9_.-]+", "_", suffix)

summary_csv <- file.path(output_dir, paste0("unskewed_sass_utility_views_", suffix, "_summary.csv"))
readr::write_csv(summary, summary_csv)
message("Summary saved to: ", summary_csv)

base_theme <- theme_bw(base_size = 24, base_family = base_font) +
	theme(
    panel.border = element_rect(linewidth = 0.8),
    panel.grid.major = element_line(linewidth = 0.65),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
		legend.text = element_text(size = 15),
    legend.key.size = unit(0.58, "cm"),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -16, 0),
		axis.text.x = element_text(size = 10),
		axis.text.y = element_text(size = 11),
		axis.title = element_text(size = 18),
		strip.text.x = element_text(size = 13, face = "bold", margin = margin(4, 0, 4, 0)),
		strip.text.y = element_text(size = 11, face = "bold", margin = margin(0, 4, 0, 4)),
		strip.background = element_rect(fill = "grey85", color = "grey20", linewidth = 0.8),
		plot.margin = margin(2, 8, 5, 8)
	)

log_breaks <- c(0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 1000000000)
log_labels <- c("0.001", "0.01", "0.1", "1", "10", "100", "1K", "10K", "100K", "1M", "10M", "1B")

plot <- ggplot(summary, aes(x = bound, y = plot_value, color = mechanism, shape = mechanism, group = mechanism)) +
	geom_line(linewidth = 0.85, na.rm = TRUE) +
	geom_point(size = 2.6, stroke = 0.8, na.rm = TRUE) +
	facet_grid(metric ~ query, scales = "free_y") +
  scale_color_manual(values = mechanism_colors, drop = TRUE) +
  scale_shape_manual(values = mechanism_shapes, drop = TRUE) +
  scale_x_log10(
    breaks = sort(unique(summary$bound)),
    labels = bound_label(sort(unique(summary$bound)))
  ) +
  scale_y_log10(breaks = log_breaks, labels = log_labels) +
	labs(x = "bound multiplier", y = "value (log scale)") +
  guides(color = guide_legend(nrow = 1, byrow = TRUE), shape = guide_legend(nrow = 1, byrow = TRUE)) +
  base_theme

plot_file <- file.path(output_dir, paste0("unskewed_sass_utility_mechanism_noise_", suffix, "_paper.png"))
png(filename = plot_file, width = 5600, height = 3150, res = 350)
print(plot)
dev.off()
message("Mechanism-target/noise plot saved to: ", plot_file)

target_summary <- summary %>%
	filter(metric == "Target error (%)")

target_plot <- ggplot(
	target_summary,
	aes(x = bound, y = plot_value, color = mechanism, shape = mechanism, group = mechanism)
) +
	geom_line(linewidth = 0.95, na.rm = TRUE) +
	geom_point(size = 3.0, stroke = 0.9, na.rm = TRUE) +
	facet_wrap(~query, nrow = 1, scales = "free_y") +
	scale_color_manual(values = mechanism_colors, drop = TRUE) +
	scale_shape_manual(values = mechanism_shapes, drop = TRUE) +
	scale_x_log10(
		breaks = sort(unique(target_summary$bound)),
		labels = bound_label(sort(unique(target_summary$bound)))
	) +
	scale_y_log10(breaks = log_breaks, labels = log_labels) +
	labs(x = "bound multiplier", y = "median target error (%)") +
	guides(color = guide_legend(nrow = 1, byrow = TRUE), shape = guide_legend(nrow = 1, byrow = TRUE)) +
	base_theme +
	theme(
		axis.text.x = element_text(size = 11),
		axis.text.y = element_text(size = 12),
		axis.title = element_text(size = 20),
		strip.text.x = element_text(size = 15, face = "bold"),
		plot.margin = margin(2, 8, 5, 8)
	)

target_plot_file <- file.path(output_dir, paste0("unskewed_sass_target_error_bound_sweep_", suffix, "_paper.png"))
png(filename = target_plot_file, width = 5600, height = 1500, res = 350)
print(target_plot)
dev.off()
message("Target-error plot saved to: ", target_plot_file)

two_error_summary <- summary %>%
	filter(metric == "Full error (%)")

two_error_plot <- ggplot(
	two_error_summary,
	aes(x = bound, y = plot_value, color = mechanism, shape = mechanism, group = mechanism)
) +
	geom_line(linewidth = 1.15, na.rm = TRUE) +
	geom_point(size = 3.6, stroke = 1.0, na.rm = TRUE) +
	facet_wrap(~query, nrow = 1, scales = "free_y") +
	scale_color_manual(values = mechanism_colors, drop = TRUE) +
	scale_shape_manual(values = mechanism_shapes, drop = TRUE) +
	scale_x_log10(
		breaks = sort(unique(two_error_summary$bound)),
		labels = bound_label(sort(unique(two_error_summary$bound)))
	) +
	scale_y_log10(breaks = log_breaks, labels = log_labels) +
	labs(x = "bound multiplier", y = "median error (%)") +
	guides(color = guide_legend(nrow = 1, byrow = TRUE), shape = guide_legend(nrow = 1, byrow = TRUE)) +
	base_theme +
	theme(
		legend.text = element_text(size = 20),
		legend.key.size = unit(0.64, "cm"),
		legend.margin = margin(0, 0, -4, 0),
		legend.box.margin = margin(0, 0, -11, 0),
		axis.text.x = element_text(size = 17, angle = 45, hjust = 1),
		axis.text.y = element_text(size = 14),
		axis.title = element_text(size = 22),
		strip.text.x = element_text(size = 19, face = "bold", margin = margin(3, 0, 3, 0)),
		plot.margin = margin(1, 6, 3, 6)
	)

two_error_plot_file <- file.path(output_dir, paste0("unskewed_sass_two_error_views_", suffix, "_paper.png"))
png(filename = two_error_plot_file, width = 4000, height = 1300, res = 350)
print(two_error_plot)
dev.off()
message("Two-error plot saved to: ", two_error_plot_file)
