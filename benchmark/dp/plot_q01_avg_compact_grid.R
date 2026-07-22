#!/usr/bin/env Rscript
# Compact Q01 AVG utility grid: AVG columns plus grouping shapes.

script_file <- sub("^--file=", "", grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "dplyr", "readr", "scales", "systemfonts", "patchwork"))

suppressPackageStartupMessages({
  library(ggplot2)
  library(dplyr)
  library(readr)
  library(scales)
  library(patchwork)
})

base_font <- PaperFont()

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 2) {
  stop("Usage: Rscript plot_q01_avg_compact_grid.R cols.csv groups.csv [output_png]")
}

cols_csv <- args[1]
groups_csv <- args[2]
output_png <- if (length(args) >= 3) args[3] else "benchmark/dp/q01_avg_compact_grid_paper.png"

mechanism_levels <- c("Bounded DP", "DP-Elastic", "SAA m=64", "SAA m=512")
mechanism_colors <- c(
  "Bounded DP" = "#d55e00",
  "DP-Elastic" = "#0072b2",
  "SAA m=64" = "#4dff4d",
  "SAA m=512" = "#009900"
)

normalize <- function(raw) {
  raw %>%
    mutate(
      success = SuccessFlag(success),
      query = as.character(query),
      release = tolower(as.character(release)),
      bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
      dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
      full_error = suppressWarnings(as.numeric(median_error_pct)),
      mechanism = case_when(
        mode == "dp_standard" ~ "Bounded DP",
        mode == "dp_elastic" ~ "DP-Elastic",
        mode == "dp_sass" & release == "average" & dp_sass_m == 64L ~ "SAA m=64",
        mode == "dp_sass" & release == "average" & dp_sass_m == 512L ~ "SAA m=512",
        TRUE ~ NA_character_
      )
    ) %>%
    filter(
      success,
      !is.na(mechanism),
      abs(bound_multiplier - 1.0) <= 1e-12,
      !is.na(full_error),
      is.finite(full_error)
    )
}

cols <- suppressWarnings(readr::read_csv(cols_csv, show_col_types = FALSE))
groups <- suppressWarnings(readr::read_csv(groups_csv, show_col_types = FALSE))

column_labels <- c(
  "q01_avg_qty_all" = "AVG(qty)",
  "q01_avg_price_all" = "AVG(price)",
  "q01_avg_disc_all" = "AVG(disc)"
)
group_labels <- c(
  "q01_avg_all" = "AVG(qty)\nAVG(price)\nAVG(disc)\nno GROUP BY",
  "q01_avg_by_linestatus" = "AVG(qty)\nAVG(price)\nAVG(disc)\nGROUP BY\nstatus",
  "q01_avg_by_returnflag" = "AVG(qty)\nAVG(price)\nAVG(disc)\nGROUP BY\nflag",
  "q01_avg_by_returnflag_linestatus" = "AVG(qty)\nAVG(price)\nAVG(disc)\nGROUP BY\nstatus, flag"
)

cols_data <- normalize(cols) %>%
	filter(query %in% names(column_labels)) %>%
	transmute(
		panel = "AVG column",
		item = factor(column_labels[query], levels = column_labels),
		mechanism = factor(mechanism, levels = mechanism_levels),
		full_error
	) %>%
	group_by(panel, item, mechanism) %>%
	summarise(
		full_error = mean(full_error, na.rm = TRUE),
		.groups = "drop"
	)

groups_data <- normalize(groups) %>%
	filter(query %in% names(group_labels)) %>%
	transmute(
		panel = "GROUP BY",
		item = factor(group_labels[query], levels = group_labels),
		mechanism = factor(mechanism, levels = mechanism_levels),
		full_error
	) %>%
	group_by(panel, item, mechanism) %>%
	summarise(
		full_error = mean(full_error, na.rm = TRUE),
		.groups = "drop"
	)

prepare_panel_data <- function(data) {
  data %>%
    mutate(
      x_center = as.numeric(item) + (as.numeric(mechanism) - 2.5) * 0.18,
      x_min = x_center - 0.075,
      x_max = x_center + 0.075,
      y_floor = 1e-5,
      plot_error = pmax(full_error, y_floor)
    )
}

cols_plot_data <- prepare_panel_data(cols_data)
groups_plot_data <- prepare_panel_data(groups_data)

plot_data <- bind_rows(cols_plot_data, groups_plot_data) %>%
  mutate(
    panel = factor(panel, levels = c("AVG column", "GROUP BY"))
  )

if (nrow(plot_data) == 0) {
  stop("No rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

base_theme <- theme_bw(base_size = 24, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.75),
    panel.grid.major = element_line(linewidth = 0.45),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.justification = "right",
    legend.title = element_blank(),
    legend.text = element_text(size = 17),
    legend.key.size = unit(0.54, "cm"),
    legend.margin = margin(0, 0, -4, 0),
    legend.box.margin = margin(0, 0, -8, 0),
    axis.text.x = element_text(size = 17),
    axis.text.y = element_text(size = 16),
    axis.title = element_text(size = 19),
    axis.title.y = element_text(margin = margin(r = 7)),
    strip.background = element_blank(),
    strip.text.y = element_blank(),
    strip.text.x = element_blank(),
    panel.spacing.y = unit(0.18, "cm"),
    plot.margin = margin(2, 8, 4, 8)
  )

make_panel <- function(data, x_hjust, show_legend, y_label, x_expand = c(0.18, 0.18), x_text_size = 17,
                       x_text_lineheight = 0.9, x_text_margin = margin()) {
  ggplot(data) +
    geom_rect(
      aes(xmin = x_min, xmax = x_max, ymin = y_floor, ymax = plot_error, fill = mechanism),
      color = "grey25",
      linewidth = 0.2
    ) +
    scale_fill_manual(values = mechanism_colors, drop = FALSE) +
    scale_x_continuous(
      breaks = seq_along(levels(droplevels(data$item))),
      labels = levels(droplevels(data$item)),
      expand = expansion(add = x_expand)
    ) +
    scale_y_log10(
      breaks = c(1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1),
      labels = c(expression(10^-5), expression(10^-4), expression(10^-3), expression(10^-2), expression(10^-1), "1"),
      expand = expansion(mult = c(0, 0.08))
    ) +
    labs(x = NULL, y = y_label) +
    guides(fill = guide_legend(nrow = 1, byrow = TRUE)) +
    coord_cartesian(clip = "off") +
    base_theme +
    theme(
      axis.text.x = element_text(
        size = x_text_size,
        hjust = x_hjust,
        lineheight = x_text_lineheight,
        margin = x_text_margin
      ),
      legend.position = if (show_legend) "top" else "none"
    )
}

top_panel <- make_panel(
  cols_plot_data,
  x_hjust = 0.5,
  show_legend = TRUE,
  y_label = NULL,
  x_text_margin = margin(t = 6, r = 0, b = 0, l = 0)
)
bottom_panel <- make_panel(
  groups_plot_data,
  x_hjust = 0.5,
  show_legend = FALSE,
  y_label = NULL,
  x_text_size = 15,
  x_text_lineheight = 0.82,
  x_text_margin = margin(t = 6, r = 0, b = 0, l = 0)
)

body_plot <- top_panel / bottom_panel + plot_layout(heights = c(1, 1.35))
y_label_plot <- ggplot() +
  annotate("text", x = 0.5, y = 0.5, label = "median error (%)", angle = 90, family = base_font, size = 8) +
  theme_void()

p <- (y_label_plot | body_plot) + plot_layout(widths = c(0.045, 1))

ggsave(output_png, p, width = 7.05, height = 4.35, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Summary saved to: ", summary_csv)
