#!/usr/bin/env Rscript
# Paper-style diagnostic plot for the TPC-H Q01 AVG-only experiment.

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
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) {
    "Linux Libertine"
  } else {
    "serif"
  }
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1) {
  stop("Usage: Rscript plot_q01_avg_groups.R results.csv [output_png]")
}

input_csv <- args[1]
output_png <- if (length(args) >= 2) args[2] else "benchmark/dp/q01_avg_groups_paper.png"

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
needed <- c(
  "query", "mode", "release", "success", "bound_multiplier", "dp_sass_m", "median_error_pct",
  "saa_estimator_median_error_pct"
)
missing <- setdiff(needed, colnames(raw))
if (length(missing) > 0) {
  stop("Missing expected columns: ", paste(missing, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

query_labels <- c(
  "q01_avg_all" = "AVG(l_quantity)\nno GROUP BY",
  "q01_avg_by_linestatus" = "AVG(l_quantity)\nGROUP BY l_linestatus",
  "q01_avg_by_returnflag" = "AVG(l_quantity)\nGROUP BY l_returnflag",
  "q01_avg_by_returnflag_linestatus" = "AVG(l_quantity)\nGROUP BY both Q1 keys"
)

mechanism_levels <- c("Bounded DP", "DP-Elastic", "SAA m=64", "SAA m=512")
mechanism_colors <- c(
  "Bounded DP" = "#d55e00",
  "DP-Elastic" = "#0072b2",
  "SAA m=64" = "#4dff4d",
  "SAA m=512" = "#009900"
)

normalized <- raw %>%
  mutate(
    success = success_flag(success),
    query = as.character(query),
    mode = as.character(mode),
    release = tolower(as.character(release)),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    error = suppressWarnings(as.numeric(median_error_pct)),
    saa_target_error = suppressWarnings(as.numeric(saa_estimator_median_error_pct)),
    mechanism = case_when(
      mode == "dp_standard" ~ "Bounded DP",
      mode == "dp_elastic" ~ "DP-Elastic",
      mode == "dp_sass" & release == "average" & dp_sass_m == 64L ~ "SAA m=64",
      mode == "dp_sass" & release == "average" & dp_sass_m == 512L ~ "SAA m=512",
      TRUE ~ NA_character_
    )
  )

plot_data <- normalized %>%
  filter(
    success,
    query %in% names(query_labels),
    !is.na(mechanism),
    abs(bound_multiplier - 1.0) <= 1e-12,
    !is.na(error),
    is.finite(error)
  ) %>%
  mutate(
    query_label = factor(query_labels[query], levels = query_labels),
    mechanism = factor(mechanism, levels = mechanism_levels)
  )

if (nrow(plot_data) == 0) {
  stop("No rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

metric_data <- bind_rows(
  plot_data %>%
    transmute(query_label, mechanism, metric = "Full answer", value = error),
  plot_data %>%
    filter(grepl("^SAA", as.character(mechanism))) %>%
    transmute(query_label, mechanism, metric = "SAA-average target", value = saa_target_error)
) %>%
  filter(!is.na(value), is.finite(value)) %>%
  mutate(
    metric = factor(metric, levels = c("Full answer", "SAA-average target")),
    display_value = ifelse(value > 0 & value < 0.025, 0.025, value),
    value_label = case_when(
      value < 0.01 ~ sprintf("%.3f", value),
      value < 0.1 ~ sprintf("%.2f", value),
      TRUE ~ sprintf("%.1f", value)
    ),
    label_visible = value < 0.04
  )

base_theme <- theme_bw(base_size = 23, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.75),
    panel.grid.major = element_line(linewidth = 0.5),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = 17),
    legend.key.size = unit(0.58, "cm"),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -12, 0),
    axis.text.x = element_text(size = 17),
    axis.text.y = element_text(size = 16),
    axis.title = element_text(size = 20),
    strip.background = element_rect(fill = "grey90", color = "grey40", linewidth = 0.7),
    strip.text = element_text(size = 18, face = "bold", margin = margin(3, 0, 3, 0)),
    plot.margin = margin(2, 8, 5, 8)
  )

p <- ggplot(metric_data, aes(x = query_label, y = display_value, fill = mechanism)) +
  geom_col(position = position_dodge(width = 0.72), width = 0.62, color = "grey25", linewidth = 0.2) +
  geom_text(
    data = metric_data %>% filter(label_visible),
    aes(label = value_label, y = display_value),
    position = position_dodge(width = 0.72),
    vjust = -0.2,
    size = 3.2,
    family = base_font,
    inherit.aes = TRUE
  ) +
  facet_grid(metric ~ ., scales = "free_y") +
  scale_fill_manual(values = mechanism_colors, drop = FALSE) +
  scale_y_continuous(labels = label_number(accuracy = 0.1), expand = expansion(mult = c(0, 0.08))) +
  labs(x = NULL, y = "median error (%)") +
  guides(fill = guide_legend(nrow = 1, byrow = TRUE)) +
  base_theme

ggsave(output_png, p, width = 9.6, height = 3.45, dpi = 300, bg = "white")

sweep_data <- normalized %>%
  filter(
    success,
    query %in% names(query_labels),
    !is.na(mechanism),
    mechanism %in% mechanism_levels,
    !is.na(bound_multiplier),
    bound_multiplier %in% c(0.01, 0.1, 1.0, 10.0, 100.0),
    !is.na(error),
    is.finite(error)
  ) %>%
  mutate(
    query_label = factor(query_labels[query], levels = query_labels),
    mechanism = factor(mechanism, levels = mechanism_levels),
    bound_label = factor(
      case_when(
        abs(bound_multiplier - 0.01) < 1e-12 ~ "0.01x",
        abs(bound_multiplier - 0.1) < 1e-12 ~ "0.1x",
        abs(bound_multiplier - 1.0) < 1e-12 ~ "1x",
        abs(bound_multiplier - 10.0) < 1e-12 ~ "10x",
        abs(bound_multiplier - 100.0) < 1e-12 ~ "100x",
        TRUE ~ paste0(bound_multiplier, "x")
      ),
      levels = c("0.01x", "0.1x", "1x", "10x", "100x")
    ),
    plot_error = pmax(error, 0.001)
  )

sweep_plot <- ggplot(sweep_data, aes(x = bound_multiplier, y = plot_error, color = mechanism, shape = mechanism, group = mechanism)) +
  geom_line(linewidth = 0.9, na.rm = TRUE) +
  geom_point(size = 2.7, stroke = 0.8, na.rm = TRUE) +
  facet_wrap(~ query_label, nrow = 1, scales = "free_y") +
  scale_color_manual(values = mechanism_colors, drop = FALSE) +
  scale_shape_manual(values = c("Bounded DP" = 16, "DP-Elastic" = 17, "SAA m=64" = 15, "SAA m=512" = 18), drop = FALSE) +
  scale_x_log10(
    breaks = c(0.01, 0.1, 1, 10, 100),
    labels = c("0.01x", "0.1x", "1x", "10x", "100x")
  ) +
  scale_y_log10(
    breaks = c(0.001, 0.01, 0.1, 1, 10),
    labels = c("0.001", "0.01", "0.1", "1", "10")
  ) +
  labs(x = "bound multiplier", y = "median full error (%)") +
  guides(color = guide_legend(nrow = 1, byrow = TRUE), shape = guide_legend(nrow = 1, byrow = TRUE)) +
  base_theme +
  theme(
    axis.text.x = element_text(size = 13, angle = 35, hjust = 1),
    strip.text = element_text(size = 15, face = "bold", margin = margin(3, 0, 3, 0))
  )

sweep_png <- sub("[.]png$", "_bound_sweep.png", output_png)
ggsave(sweep_png, sweep_plot, width = 9.6, height = 2.7, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Bound sweep plot saved to: ", sweep_png)
message("Summary saved to: ", summary_csv)
