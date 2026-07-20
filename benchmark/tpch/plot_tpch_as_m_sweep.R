#!/usr/bin/env Rscript
# Paper-style TPC-H AS m-sweep runtime plot.
# Input CSVs are produced by as_tpch_benchmark:
#   query,mode,m,median_ms

script_file <- sub("^--file=", "", grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "ggpattern", "dplyr", "readr", "scales", "stringr", "systemfonts"))

suppressPackageStartupMessages({
  library(ggplot2)
  library(ggpattern)
  library(dplyr)
  library(readr)
  library(scales)
  library(stringr)
})

base_font <- PaperFont()

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_tpch_as_m_sweep.R input.csv [output_dir] [more_input.csv ...]")
}

is_csv_arg <- function(x) grepl("\\.csv$", x, ignore.case = TRUE)
input_csvs <- args[is_csv_arg(args)]
if (length(input_csvs) == 0) {
  stop("No input CSVs provided.")
}
output_dir_arg <- args[!is_csv_arg(args)]
output_dir <- if (length(output_dir_arg) >= 1) output_dir_arg[1] else dirname(input_csvs[1])
if (!dir.exists(output_dir)) dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)

drop_m_env <- Sys.getenv("DROP_M")
drop_m_values <- integer()
if (nzchar(drop_m_env)) {
  drop_m_values <- as.integer(strsplit(drop_m_env, ",", fixed = TRUE)[[1]])
  drop_m_values <- drop_m_values[!is.na(drop_m_values)]
}

read_one <- function(path) {
  df <- suppressWarnings(readr::read_csv(path, show_col_types = FALSE))
  expected_cols <- c("query", "mode", "median_ms")
  missing_cols <- setdiff(expected_cols, colnames(df))
  if (length(missing_cols) > 0) {
    stop("Missing expected columns in ", path, ": ", paste(missing_cols, collapse = ", "))
  }
  if (!("m" %in% colnames(df))) {
    df$m <- ifelse(df$mode == "SIMD AS", 64L, 0L)
  }
  df %>% mutate(source_file = basename(path))
}

raw <- bind_rows(lapply(input_csvs, read_one)) %>%
  mutate(
    query = as.character(query),
    mode = as.character(mode),
    m = as.integer(m),
    median_ms = as.numeric(median_ms),
    qnum = as.integer(str_extract(query, "\\d+")),
    failed = median_ms < 0
  ) %>%
  filter(!is.na(qnum), mode %in% c("baseline", "SIMD AS"), is.finite(median_ms))

if (nrow(raw) == 0) {
  stop("No plottable rows found.")
}

raw <- raw %>%
  filter(query == sprintf("q%02d", qnum)) %>%
  group_by(query, qnum, mode, m) %>%
  summarize(
    median_ms = median(median_ms, na.rm = TRUE),
    failed = any(failed),
    source_count = n_distinct(source_file),
    .groups = "drop"
  )

baseline_df <- raw %>%
  filter(mode == "baseline") %>%
  transmute(query, qnum, baseline_ms = median_ms)

as_data <- raw %>%
	filter(mode == "SIMD AS") %>%
	inner_join(baseline_df, by = c("query", "qnum")) %>%
	filter(baseline_ms > 0, median_ms > 0, !failed) %>%
	filter(!(m %in% drop_m_values)) %>%
	mutate(
		slowdown = median_ms / baseline_ms,
    query_label = paste0("Q", qnum),
    m_label = factor(paste0("m=", m), levels = paste0("m=", sort(unique(m))))
	) %>%
	arrange(qnum, m)

if (nrow(as_data) == 0) {
	stop("No successful SIMD-AS rows found.")
}

query_levels <- as_data %>%
	distinct(qnum, query_label) %>%
	arrange(qnum) %>%
	pull(query_label)

m_levels <- levels(as_data$m_label)
m_palette_all <- c(
	"DuckDB" = "#95a5a6",
	"m=64" = "#4dff4d",
	"m=128" = "#b7e4c7",
	"m=256" = "#52b788",
	"m=512" = "#009900"
)
mode_levels <- c("DuckDB", m_levels)
missing_colors <- setdiff(mode_levels, names(m_palette_all))
if (length(missing_colors) > 0) {
	extra <- scales::hue_pal()(length(missing_colors))
	names(extra) <- missing_colors
	m_palette_all <- c(m_palette_all, extra)
}
m_palette <- m_palette_all[mode_levels]
m_patterns_all <- c("DuckDB" = "none", "m=64" = "none", "m=128" = "stripe", "m=256" = "crosshatch", "m=512" = "stripe")
m_pattern_angles_all <- c("DuckDB" = 0, "m=64" = 0, "m=128" = 45, "m=256" = 135, "m=512" = 0)
m_patterns <- m_patterns_all[mode_levels]
m_pattern_angles <- m_pattern_angles_all[mode_levels]

summary_df <- as_data %>%
	group_by(m_label) %>%
	summarize(
		queries = n(),
		median_slowdown = median(slowdown),
    mean_slowdown = mean(slowdown),
    within_2x = sum(slowdown <= 2),
    within_3x = sum(slowdown <= 3),
    max_slowdown = max(slowdown),
    max_query = query_label[which.max(slowdown)],
    .groups = "drop"
  )

message("\n=== TPC-H AS m-sweep slowdown summary ===")
for (i in seq_len(nrow(summary_df))) {
  r <- summary_df[i, ]
  message(sprintf(
    "%s: median %.2fx, mean %.2fx, <=2x %d/%d, <=3x %d/%d, max %.2fx (%s)",
    r$m_label, r$median_slowdown, r$mean_slowdown, r$within_2x, r$queries,
    r$within_3x, r$queries, r$max_slowdown, r$max_query
  ))
}
message("========================================\n")

summary_file <- file.path(output_dir, "tpch_as_m_sweep_slowdown_paper_summary.csv")
readr::write_csv(summary_df, summary_file)

baseline_plot <- baseline_df %>%
	filter(baseline_ms > 0) %>%
	mutate(
		median_ms = baseline_ms,
		slowdown = NA_real_,
		query_label = paste0("Q", qnum),
		m_label = factor("DuckDB", levels = mode_levels)
	) %>%
	select(query, qnum, median_ms, slowdown, query_label, m_label)

plot_data <- bind_rows(
	baseline_plot,
	as_data %>%
		mutate(m_label = factor(as.character(m_label), levels = mode_levels)) %>%
		select(query, qnum, median_ms, slowdown, query_label, m_label)
) %>%
	filter(query_label %in% query_levels) %>%
	mutate(m_label = factor(as.character(m_label), levels = mode_levels))

label_data <- plot_data %>%
	mutate(
		label = ifelse(
			as.character(m_label) == "m=512" & is.finite(slowdown),
			ifelse(slowdown >= 10, sprintf("%.0fx", slowdown), sprintf("%.1fx", slowdown)),
			""
		),
		y_pos = median_ms * 1.10
	)

y_upper <- max(c(plot_data$median_ms, label_data$y_pos), na.rm = TRUE) * 1.45

p <- ggplot(plot_data, aes(x = factor(query_label, levels = query_levels), y = median_ms, fill = m_label, pattern = m_label, pattern_angle = m_label)) +
	geom_col_pattern(
		position = position_dodge(width = 0.86),
		width = 0.72,
		color = NA,
		pattern_fill = "black",
		pattern_colour = "black",
		pattern_alpha = 0.25,
		pattern_density = 0.10,
		pattern_spacing = 0.045,
		pattern_key_scale_factor = 0.45
	) +
	geom_text(
		data = label_data,
		aes(x = factor(query_label, levels = query_levels), label = label, y = y_pos, group = m_label),
    position = position_dodge(width = 0.86),
    size = 4.4,
    vjust = 0,
    fontface = "bold",
    family = base_font
	) +
	scale_fill_manual(values = m_palette, name = NULL) +
	scale_pattern_manual(values = m_patterns, name = NULL) +
	scale_pattern_angle_manual(values = m_pattern_angles, guide = "none") +
	guides(fill = guide_legend(override.aes = list(pattern = unname(m_patterns), pattern_angle = unname(m_pattern_angles)))) +
	scale_y_log10(
		labels = function(x) ifelse(x >= 100, paste0(x / 1000, "s"), paste0(x, "ms"))
	) +
	scale_x_discrete(drop = FALSE, expand = expansion(add = c(0.6, 0.66))) +
	coord_cartesian(ylim = c(NA, y_upper), clip = "off") +
	labs(x = NULL, y = NULL) +
	theme_bw(base_size = 40, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 1.0),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 29),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -20, 0),
    axis.text.x = element_text(angle = 45, hjust = 1, size = 25),
    axis.text.y = element_text(size = 25),
    axis.title = element_blank(),
    plot.title = element_blank(),
	plot.margin = margin(2, 8, 5, 5)
	)

out_file <- file.path(output_dir, "tpch_as_m_sweep_paper.png")
png(filename = out_file, width = 4000, height = 1500, res = 350)
print(p)
dev.off()

message("font: ", base_font)
message("input CSVs: ", paste(input_csvs, collapse = ", "))
if (length(drop_m_values) > 0) {
	message("dropped m values: ", paste(drop_m_values, collapse = ", "))
}
message("summary saved to: ", summary_file)
message("plot saved to: ", out_file)
