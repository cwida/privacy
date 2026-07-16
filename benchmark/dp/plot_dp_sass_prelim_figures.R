#!/usr/bin/env Rscript
# Preliminary DP-SAA utility figures from mixed result sources:
# - new/partial unskewed EC2 results
# - older controlled-skew results
# - local delta-sweep results

user_lib <- Sys.getenv("R_LIBS_USER")
if (user_lib == "") {
  user_lib <- file.path(Sys.getenv("HOME"), "R", "libs")
}
if (!dir.exists(user_lib)) {
  dir.create(user_lib, recursive = TRUE, showWarnings = FALSE)
}
.libPaths(c(user_lib, .libPaths()))

required_packages <- c("ggplot2", "dplyr", "readr", "scales", "stringr", "tidyr", "systemfonts", "grid")
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
  library(stringr)
  library(tidyr)
  library(grid)
})

base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) {
    "Linux Libertine"
  } else {
    "serif"
  }
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
unskewed_csv <- if (length(args) >= 1) args[1] else "/tmp/dp_new_bounds_unskewed/unskewed_new_bounds_results.csv"
skew_dir <- if (length(args) >= 2) args[2] else "benchmark/dp/skew_sweep"
delta_csv <- if (length(args) >= 3) args[3] else "/tmp/tpch_sf10_all5_delta_sweep_m64_m512_1run_results.csv"
output_dir <- if (length(args) >= 4) args[4] else "/tmp/dp_sass_prelim_figures"

if (!dir.exists(output_dir)) {
  dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
}

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
bound_levels <- c("0.01x", "0.1x", "1x", "10x", "100x")
delta_levels <- c("1e-08", "1e-07", "1e-06", "1e-05", "1e-04")

mechanism_levels <- c(
  "DP standard", "DP elastic",
  "SAA avg m=64", "SAA med m=64",
  "SAA avg m=512", "SAA med m=512"
)

mechanism_colors <- c(
  "DP standard" = "#95a5a6",
  "DP elastic" = "#a8d4ff",
  "SAA avg m=64" = "#66bd63",
  "SAA med m=64" = "#b2df8a",
  "SAA avg m=512" = "#009900",
  "SAA med m=512" = "#4dff4d",
  "SAA average" = "#009900",
  "SAA median" = "#4dff4d"
)

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

format_bound <- function(x) {
  dplyr::case_when(
    abs(x - 0.01) < 1e-12 ~ "0.01x",
    abs(x - 0.1) < 1e-12 ~ "0.1x",
    abs(x - 1.0) < 1e-12 ~ "1x",
    abs(x - 10.0) < 1e-12 ~ "10x",
    abs(x - 100.0) < 1e-12 ~ "100x",
    TRUE ~ paste0(format(x, trim = TRUE, scientific = FALSE), "x")
  )
}

format_delta <- function(x) {
  formatC(as.numeric(x), format = "e", digits = 0)
}

variant_label_one <- function(variant) {
  if (is.na(variant) || variant == "") {
    return(NA_character_)
  }
  if (variant == "unskewed") {
    return("unskewed TPC-H")
  }
  match <- stringr::str_match(variant, "r([0-9]+)_w([0-9]+)")
  if (is.na(match[1, 1])) {
    return(variant)
  }
  paste0("remap ", match[1, 2], "%, hot ", match[1, 3])
}
variant_label <- function(variant) {
  vapply(variant, variant_label_one, character(1))
}

read_result_csv <- function(path, variant, source_label) {
  if (!file.exists(path)) {
    stop("Missing result CSV: ", path)
  }
  raw <- suppressWarnings(readr::read_csv(path, show_col_types = FALSE))
  needed <- c("query", "mode", "release", "success", "median_error_pct", "bound_multiplier", "dp_sass_m", "run")
  missing <- setdiff(needed, colnames(raw))
  if (length(missing) > 0) {
    stop("Missing columns in ", path, ": ", paste(missing, collapse = ", "))
  }
  raw %>%
    mutate(
      source_file = path,
      source = source_label,
      variant = variant,
      variant_label = variant_label(variant),
      dataset = toupper(dataset),
      query = toupper(query),
      success = success_flag(success),
      run = suppressWarnings(as.integer(run)),
      bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
      bound_label = factor(format_bound(bound_multiplier), levels = bound_levels),
      dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
      median_error_pct = suppressWarnings(as.numeric(median_error_pct)),
      utility = suppressWarnings(as.numeric(utility)),
      mechanism = case_when(
        mode == "dp_standard" ~ "DP standard",
        mode == "dp_elastic" ~ "DP elastic",
        mode == "dp_sass" & release == "average" & dp_sass_m == 64 ~ "SAA avg m=64",
        mode == "dp_sass" & release == "median" & dp_sass_m == 64 ~ "SAA med m=64",
        mode == "dp_sass" & release == "average" & dp_sass_m == 512 ~ "SAA avg m=512",
        mode == "dp_sass" & release == "median" & dp_sass_m == 512 ~ "SAA med m=512",
        TRUE ~ NA_character_
      )
    ) %>%
    filter(
      success,
      query %in% query_levels,
      bound_label %in% bound_levels,
      !is.na(mechanism),
      !is.na(median_error_pct),
      is.finite(median_error_pct)
    ) %>%
    mutate(
      query = factor(query, levels = query_levels),
      mechanism = factor(mechanism, levels = mechanism_levels)
    )
}

skew_files <- list.files(
  skew_dir,
  pattern = "^controlled_skew_sf30_r[0-9]+_w[0-9]+_sequential_3run_results[.]csv$",
  full.names = TRUE
)
if (length(skew_files) == 0) {
  stop("No controlled-skew result CSVs found in ", skew_dir)
}

unskewed <- read_result_csv(unskewed_csv, "unskewed", "partial EC2 unskewed/new bounds")
skewed <- bind_rows(lapply(skew_files, function(path) {
  variant <- stringr::str_match(basename(path), "controlled_skew_sf30_(r[0-9]+_w[0-9]+)_sequential")[1, 2]
  read_result_csv(path, variant, "old controlled-skew")
}))
all_results <- bind_rows(unskewed, skewed)

per_query <- all_results %>%
  group_by(source, variant, variant_label, query, bound_label, bound_multiplier, mechanism) %>%
  summarize(
    runs = n(),
    median_error_pct = median(median_error_pct, na.rm = TRUE),
    mean_error_pct = mean(median_error_pct, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(plot_error_pct = pmax(median_error_pct, 0.001))

winner_detail <- per_query %>%
  group_by(source, variant, variant_label, query, bound_label, bound_multiplier) %>%
  arrange(median_error_pct, .by_group = TRUE) %>%
  slice_head(n = 1) %>%
  ungroup() %>%
  mutate(winner = as.character(mechanism))

winner_summary_all <- winner_detail %>%
  count(source, variant, variant_label, bound_label, bound_multiplier, winner, name = "query_count") %>%
  complete(
    nesting(source, variant, variant_label, bound_label, bound_multiplier),
    winner = mechanism_levels,
    fill = list(query_count = 0)
  ) %>%
  mutate(winner = factor(winner, levels = mechanism_levels))

readr::write_csv(per_query, file.path(output_dir, "prelim_per_query_errors.csv"))
readr::write_csv(winner_detail, file.path(output_dir, "prelim_winner_detail.csv"))
readr::write_csv(winner_summary_all, file.path(output_dir, "prelim_winner_summary_all_variants.csv"))

selected_variants <- c("unskewed", "r10_w50", "r60_w50", "r60_w450")
winner_summary <- winner_summary_all %>%
  filter(variant %in% selected_variants) %>%
  mutate(
    variant_label = factor(
      variant_label,
      levels = variant_label(selected_variants)
    )
  )

base_theme <- theme_bw(base_size = 30, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.8),
    panel.grid.major = element_line(linewidth = 0.55),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 14),
    legend.key.size = unit(0.48, "cm"),
    legend.margin = margin(0, 0, -2, 0),
    legend.box.margin = margin(0, 0, -12, 0),
    axis.text.x = element_text(size = 13),
    axis.text.y = element_text(size = 14),
    axis.title = element_text(size = 21),
    strip.text = element_text(size = 15, face = "bold", margin = margin(3, 0, 3, 0)),
    strip.background = element_rect(fill = "grey88", color = "grey35", linewidth = 0.7),
    plot.title = element_text(size = 22, face = "bold", hjust = 0.5, margin = margin(0, 0, 4, 0)),
    plot.margin = margin(2, 5, 5, 5)
  )

log_breaks <- c(0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000, 1000000)
log_labels <- c("0.001", "0.01", "0.1", "1", "10", "100", "1K", "10K", "100K", "1M")

winner_plot <- ggplot(winner_summary, aes(x = bound_label, y = query_count, fill = winner)) +
  geom_col(width = 0.72, color = "white", linewidth = 0.15) +
  facet_wrap(~ variant_label, nrow = 1) +
  scale_fill_manual(values = mechanism_colors, drop = FALSE) +
  scale_y_continuous(breaks = 0:5, limits = c(0, 5), expand = expansion(mult = c(0, 0.04))) +
  labs(title = "Best end-to-end error by query", x = "bound multiplier", y = "queries won (of 5)") +
  base_theme +
  theme(axis.text.x = element_text(size = 11, angle = 35, hjust = 1))

winner_plot_file <- file.path(output_dir, "dp_sass_prelim_winner_summary.png")
png(filename = winner_plot_file, width = 5600, height = 1600, res = 350)
print(winner_plot)
dev.off()
message("Winner plot saved to: ", winner_plot_file)

read_stability <- function(path, variant, source_label) {
  if (!file.exists(path)) {
    return(tibble())
  }
  suppressWarnings(readr::read_csv(path, show_col_types = FALSE)) %>%
    mutate(
      source = source_label,
      variant = variant,
      variant_label = variant_label(variant),
      query = toupper(query),
      success = success_flag(success),
      bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
      median_cv = suppressWarnings(as.numeric(median_cv))
    ) %>%
    filter(success, query %in% query_levels, abs(bound_multiplier - 1.0) < 1e-12, !is.na(median_cv)) %>%
    group_by(source, variant, variant_label, query) %>%
    summarize(median_cv = median(median_cv, na.rm = TRUE), .groups = "drop")
}

stability_tpch <- read_stability("benchmark/dp/tpch_sf30_all5_sass_stability_summary.csv", "unskewed", "partial EC2 unskewed/new bounds")
stability_skew <- if (file.exists(file.path(skew_dir, "controlled_skew_sf30_stability_summary.csv"))) {
  suppressWarnings(readr::read_csv(file.path(skew_dir, "controlled_skew_sf30_stability_summary.csv"), show_col_types = FALSE)) %>%
    mutate(
      source = "old controlled-skew",
      variant = stringr::str_match(db, "controlled_skew_sf30_(r[0-9]+_w[0-9]+)[.]db")[, 2],
      variant_label = variant_label(variant),
      query = toupper(query),
      success = success_flag(success),
      bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
      median_cv = suppressWarnings(as.numeric(median_cv))
    ) %>%
    filter(
      success,
      !is.na(variant),
      query %in% query_levels,
      abs(bound_multiplier - 1.0) < 1e-12,
      !is.na(median_cv)
    ) %>%
    group_by(source, variant, variant_label, query) %>%
    summarize(median_cv = median(median_cv, na.rm = TRUE), .groups = "drop")
} else {
  tibble()
}
stability <- bind_rows(stability_tpch, stability_skew)

advantage <- per_query %>%
  filter(bound_label == "1x") %>%
  mutate(family = case_when(
    mechanism %in% c("DP standard", "DP elastic") ~ "DP",
    mechanism %in% c("SAA avg m=512", "SAA med m=512") ~ "SAA",
    TRUE ~ "other"
  )) %>%
  filter(family %in% c("DP", "SAA")) %>%
  group_by(source, variant, variant_label, query, family) %>%
  summarize(best_error_pct = min(median_error_pct, na.rm = TRUE), .groups = "drop") %>%
  tidyr::pivot_wider(names_from = family, values_from = best_error_pct) %>%
  filter(!is.na(DP), !is.na(SAA), DP > 0, SAA > 0) %>%
  mutate(saa_vs_dp_log10 = log10(SAA / DP)) %>%
  left_join(stability, by = c("source", "variant", "variant_label", "query")) %>%
  filter(!is.na(median_cv))

readr::write_csv(advantage, file.path(output_dir, "prelim_stability_advantage.csv"))

stability_plot <- ggplot(
  advantage %>% filter(variant %in% selected_variants),
  aes(x = median_cv, y = saa_vs_dp_log10, color = variant_label, shape = query)
) +
  geom_hline(yintercept = 0, linetype = "dashed", linewidth = 0.6, color = "grey35") +
  geom_point(size = 3.2, alpha = 0.9) +
  scale_x_log10(labels = label_number(accuracy = 0.001)) +
  scale_y_continuous(labels = label_number(accuracy = 0.1)) +
  scale_color_manual(
    values = c(
      "unskewed TPC-H" = "#555555",
      "remap 10%, hot 50" = "#0072b2",
      "remap 60%, hot 50" = "#d55e00",
      "remap 60%, hot 450" = "#cc79a7"
    ),
    drop = FALSE
  ) +
  labs(
    title = "Stability vs SAA advantage",
    x = "lane CV",
    y = expression(log[10]("best SAA / best DP error"))
  ) +
  base_theme +
  guides(color = guide_legend(nrow = 2, byrow = TRUE), shape = guide_legend(nrow = 1)) +
  theme(legend.text = element_text(size = 11))

stability_plot_file <- file.path(output_dir, "dp_sass_prelim_stability_advantage.png")
png(filename = stability_plot_file, width = 3000, height = 1800, res = 350)
print(stability_plot)
dev.off()
message("Stability plot saved to: ", stability_plot_file)

unskewed_views <- unskewed %>%
  filter(variant == "unskewed", bound_label == "1x") %>%
  mutate(
    mechanism_short = case_when(
      mechanism == "DP standard" ~ "DP standard",
      mechanism == "DP elastic" ~ "DP elastic",
      mechanism == "SAA avg m=64" ~ "SAA avg m=64",
      mechanism == "SAA med m=64" ~ "SAA med m=64",
      mechanism == "SAA avg m=512" ~ "SAA avg m=512",
      mechanism == "SAA med m=512" ~ "SAA med m=512",
      TRUE ~ as.character(mechanism)
    ),
    full_answer_error_pct = median_error_pct,
    mechanism_error_pct = case_when(
      mode == "dp_sass" ~ suppressWarnings(as.numeric(saa_estimator_median_error_pct)),
      mode %in% c("dp_standard", "dp_elastic") ~ median_error_pct,
      TRUE ~ NA_real_
    ),
    sampling_error_pct = suppressWarnings(as.numeric(saa_sampling_median_error_pct)),
    noise_scale = suppressWarnings(as.numeric(saa_noise_scale_median))
  )

view_summary <- unskewed_views %>%
  group_by(query, mechanism_short) %>%
  summarize(
    full_answer_error_pct = median(full_answer_error_pct, na.rm = TRUE),
    mechanism_error_pct = median(mechanism_error_pct, na.rm = TRUE),
    sampling_error_pct = median(sampling_error_pct, na.rm = TRUE),
    noise_scale = median(noise_scale, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(
    query = factor(as.character(query), levels = query_levels),
    mechanism_short = factor(mechanism_short, levels = mechanism_levels)
  )
readr::write_csv(view_summary, file.path(output_dir, "prelim_unskewed_error_views_bound1x.csv"))

error_long <- view_summary %>%
  select(query, mechanism_short, full_answer_error_pct, mechanism_error_pct) %>%
  tidyr::pivot_longer(
    cols = c(full_answer_error_pct, mechanism_error_pct),
    names_to = "view",
    values_to = "error_pct"
  ) %>%
  mutate(
    view = recode(
      view,
      "full_answer_error_pct" = "against full answer",
      "mechanism_error_pct" = "against estimator/noiseless answer"
    ),
    plot_error_pct = pmax(error_pct, 0.001)
  ) %>%
  filter(!is.na(error_pct), is.finite(error_pct))

noise_long <- view_summary %>%
  filter(mechanism_short %in% c("SAA avg m=64", "SAA med m=64", "SAA avg m=512", "SAA med m=512")) %>%
  mutate(plot_noise_scale = pmax(noise_scale, 0.001)) %>%
  filter(!is.na(noise_scale), is.finite(noise_scale))

error_plot <- ggplot(error_long, aes(x = query, y = plot_error_pct, fill = mechanism_short)) +
  geom_col(position = position_dodge2(width = 0.86, preserve = "single"), width = 0.74, na.rm = TRUE) +
  facet_wrap(~ view, nrow = 1) +
  scale_fill_manual(values = mechanism_colors, drop = FALSE) +
  scale_y_log10(breaks = log_breaks, labels = log_labels) +
  labs(title = "Unskewed utility views", x = NULL, y = "median error (%)") +
  base_theme +
  guides(fill = guide_legend(nrow = 2, byrow = TRUE)) +
  theme(axis.text.x = element_text(size = 12))

noise_plot <- ggplot(noise_long, aes(x = query, y = plot_noise_scale, fill = mechanism_short)) +
	geom_col(position = position_dodge2(width = 0.82, preserve = "single"), width = 0.72, na.rm = TRUE) +
	scale_fill_manual(values = mechanism_colors, drop = FALSE) +
	scale_y_log10(labels = label_number(scale_cut = cut_short_scale())) +
	labs(title = "SAA raw noise scale", x = NULL, y = "median scale") +
	base_theme +
	guides(fill = guide_legend(nrow = 2, byrow = TRUE)) +
	theme(axis.text.x = element_text(size = 12), legend.position = "none")

error_views_file <- file.path(output_dir, "dp_sass_prelim_unskewed_error_views.png")
png(filename = error_views_file, width = 5200, height = 1900, res = 350)
grid.newpage()
pushViewport(viewport(layout = grid.layout(1, 2, widths = unit(c(1.45, 0.85), "null"))))
print(error_plot, vp = viewport(layout.pos.row = 1, layout.pos.col = 1))
print(noise_plot, vp = viewport(layout.pos.row = 1, layout.pos.col = 2))
dev.off()
message("Error/noise plot saved to: ", error_views_file)

delta_raw <- suppressWarnings(readr::read_csv(delta_csv, show_col_types = FALSE))
delta_needed <- c("query", "mode", "release", "success", "median_error_pct", "saa_estimator_median_error_pct", "delta", "dp_sass_m")
missing_delta <- setdiff(delta_needed, colnames(delta_raw))
if (length(missing_delta) > 0) {
  stop("Missing columns in delta CSV: ", paste(missing_delta, collapse = ", "))
}

delta_summary <- delta_raw %>%
  mutate(
    query = toupper(query),
    success = success_flag(success),
    delta = suppressWarnings(as.numeric(delta)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    full_error = suppressWarnings(as.numeric(median_error_pct)),
    estimator_error = suppressWarnings(as.numeric(saa_estimator_median_error_pct)),
    mechanism = case_when(
      mode == "dp_standard" ~ "DP standard",
      mode == "dp_elastic" ~ "DP elastic",
      mode == "dp_sass" & release == "average" & dp_sass_m == 64 ~ "SAA avg m=64",
      mode == "dp_sass" & release == "median" & dp_sass_m == 64 ~ "SAA med m=64",
      mode == "dp_sass" & release == "average" & dp_sass_m == 512 ~ "SAA avg m=512",
      mode == "dp_sass" & release == "median" & dp_sass_m == 512 ~ "SAA med m=512",
      TRUE ~ NA_character_
    ),
    mechanism_error = if_else(mode == "dp_sass", estimator_error, full_error)
  ) %>%
  filter(success, query %in% query_levels, !is.na(delta), !is.na(mechanism), !is.na(mechanism_error)) %>%
  group_by(delta, mechanism) %>%
  summarize(
    queries = n_distinct(query),
    median_mechanism_error_pct = median(mechanism_error, na.rm = TRUE),
    median_full_error_pct = median(full_error, na.rm = TRUE),
    .groups = "drop"
  ) %>%
  mutate(
    delta_label = factor(format_delta(delta), levels = delta_levels),
    mechanism = factor(mechanism, levels = mechanism_levels),
    plot_mechanism_error_pct = pmax(median_mechanism_error_pct, 0.001)
  )

readr::write_csv(delta_summary, file.path(output_dir, "prelim_delta_sensitivity_summary.csv"))

delta_plot <- ggplot(delta_summary, aes(x = delta, y = plot_mechanism_error_pct, color = mechanism, group = mechanism)) +
	geom_line(linewidth = 1.0, na.rm = TRUE) +
	geom_point(size = 2.7, na.rm = TRUE) +
	scale_color_manual(
		values = mechanism_colors,
		labels = c(
			"DP standard" = "DP std",
			"DP elastic" = "DP elastic",
			"SAA avg m=64" = "Avg64",
			"SAA med m=64" = "Med64",
			"SAA avg m=512" = "Avg512",
			"SAA med m=512" = "Med512"
		),
		drop = FALSE
	) +
  scale_x_log10(
    breaks = c(1e-8, 1e-7, 1e-6, 1e-5, 1e-4),
    labels = c("1e-8", "1e-7", "1e-6", "1e-5", "1e-4")
  ) +
  scale_y_log10(breaks = log_breaks, labels = log_labels) +
	labs(title = expression(delta * " sensitivity (SF10)"), x = expression(delta), y = "median mechanism error (%)") +
	base_theme +
	guides(color = guide_legend(nrow = 3, byrow = TRUE)) +
	theme(axis.text.x = element_text(size = 12), legend.text = element_text(size = 11))

delta_plot_file <- file.path(output_dir, "dp_sass_prelim_delta_sensitivity.png")
png(filename = delta_plot_file, width = 3000, height = 1800, res = 350)
print(delta_plot)
dev.off()
message("Delta plot saved to: ", delta_plot_file)

composite_file <- file.path(output_dir, "dp_sass_prelim_three_panel_grid.png")
png(filename = composite_file, width = 7200, height = 2300, res = 350)
grid.newpage()
pushViewport(viewport(layout = grid.layout(1, 3, widths = unit(c(1.65, 1, 1), "null"))))
print(
	winner_plot +
		guides(fill = guide_legend(nrow = 3, byrow = TRUE)) +
		theme(legend.position = "top", legend.text = element_text(size = 10)),
	vp = viewport(layout.pos.row = 1, layout.pos.col = 1)
)
print(
	stability_plot +
		guides(color = guide_legend(nrow = 2, byrow = TRUE), shape = guide_legend(nrow = 1)) +
		theme(legend.position = "top", legend.text = element_text(size = 9)),
	vp = viewport(layout.pos.row = 1, layout.pos.col = 2)
)
print(delta_plot + theme(legend.position = "none"), vp = viewport(layout.pos.row = 1, layout.pos.col = 3))
dev.off()
message("Composite plot saved to: ", composite_file)

note_file <- file.path(output_dir, "README.txt")
writeLines(c(
  "Preliminary DP-SAA paper-figure inputs",
  "",
  paste0("Unskewed source: ", unskewed_csv),
  paste0("Skew source directory: ", skew_dir),
  paste0("Delta source: ", delta_csv),
  "",
  "Caveat: the skewed result CSVs are older controlled-skew runs and may not use the same final bounds as the unskewed partial EC2 result.",
  "The winner summaries count the lowest median_error_pct per query/bound cell among DP standard, DP elastic, SAA average, and SAA median at m=64 and m=512.",
  "The stability panel uses median_cv from the stability summaries and compares best SAA m=512 error to the best DP error at the 1x bound multiplier.",
  "The delta panel uses the local SF10 sweep and reports mechanism error: for DP modes, private output vs full answer; for SAA, private output vs the corresponding non-private SAA estimator."
), note_file)
message("Notes saved to: ", note_file)
