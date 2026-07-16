#!/usr/bin/env Rscript
# Paper-style Q01 AVG-column diagnostic: one point per AVG column, SAA m=512 only.

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
  stop("Usage: Rscript plot_q01_avg_cols_m512.R results.csv [output_png]")
}

input_csv <- args[1]
output_png <- if (length(args) >= 2) args[2] else "benchmark/dp/q01_avg_cols_m512_paper.png"

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
needed <- c("query", "mode", "release", "success", "bound_multiplier", "dp_sass_m", "median_error_pct")
missing <- setdiff(needed, colnames(raw))
if (length(missing) > 0) {
  stop("Missing expected columns: ", paste(missing, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

column_labels <- c(
  "q01_avg_qty_all" = "AVG(quantity)",
  "q01_avg_price_all" = "AVG(price)",
  "q01_avg_disc_all" = "AVG(discount)"
)

mechanism_levels <- c("Bounded DP", "DP-Elastic", "SAA m=512")
mechanism_colors <- c(
  "Bounded DP" = "#d55e00",
  "DP-Elastic" = "#0072b2",
  "SAA m=512" = "#009900"
)
mechanism_shapes <- c(
  "Bounded DP" = 16,
  "DP-Elastic" = 17,
  "SAA m=512" = 15
)

plot_data <- raw %>%
  mutate(
    success = success_flag(success),
    query = as.character(query),
    release = tolower(as.character(release)),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    full_error = suppressWarnings(as.numeric(median_error_pct)),
    mechanism = case_when(
      mode == "dp_standard" ~ "Bounded DP",
      mode == "dp_elastic" ~ "DP-Elastic",
      mode == "dp_sass" & release == "average" & dp_sass_m == 512L ~ "SAA m=512",
      TRUE ~ NA_character_
    )
  ) %>%
  filter(
    success,
    query %in% names(column_labels),
    !is.na(mechanism),
    abs(bound_multiplier - 1.0) <= 1e-12,
    !is.na(full_error),
    is.finite(full_error)
  ) %>%
  mutate(
    avg_column = factor(column_labels[query], levels = column_labels),
    mechanism = factor(mechanism, levels = mechanism_levels),
    plot_error = pmax(full_error, 0.0001)
  )

if (nrow(plot_data) == 0) {
  stop("No rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

base_theme <- theme_bw(base_size = 22, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.75),
    panel.grid.major = element_line(linewidth = 0.45),
    panel.grid.minor = element_line(linewidth = 0.25, color = "grey88"),
    legend.position = "top",
    legend.justification = "center",
    legend.title = element_blank(),
    legend.text = element_text(size = 16),
    legend.key.size = unit(0.52, "cm"),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -9, 0),
    axis.text.x = element_text(size = 16),
    axis.text.y = element_text(size = 15),
    axis.title = element_text(size = 17),
    axis.title.y = element_text(margin = margin(r = 8)),
    plot.margin = margin(3, 8, 5, 34)
  )

p <- ggplot(plot_data, aes(x = avg_column, y = plot_error, color = mechanism, shape = mechanism)) +
  geom_hline(yintercept = c(0.001, 0.01, 0.1, 1), color = "grey72", linewidth = 0.3) +
  geom_point(position = position_dodge(width = 0.48), size = 3.1, stroke = 0.8) +
  scale_color_manual(values = mechanism_colors, drop = FALSE) +
  scale_shape_manual(values = mechanism_shapes, drop = FALSE) +
  scale_y_log10(
    breaks = c(0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1),
    labels = c("0.0005", "0.001", "0.005", "0.010", "0.050", "0.100", "0.500", "1.000")
  ) +
  labs(x = NULL, y = "median error (%)") +
  guides(
    color = guide_legend(nrow = 1, byrow = TRUE),
    shape = guide_legend(nrow = 1, byrow = TRUE)
  ) +
  coord_cartesian(clip = "off") +
  base_theme

ggsave(output_png, p, width = 7.05, height = 2.65, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Summary saved to: ", summary_csv)
