#!/usr/bin/env Rscript
# Paper-style DP-SASS utility panel over m and delta.

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
  stop("Usage: Rscript plot_sass_m_delta_panel.R results.csv [output_png] [dataset=tpch] [bound_multiplier=1]")
}

input_csv <- args[1]
output_png <- if (length(args) >= 2) args[2] else "benchmark/dp/tpch_sass_m_delta_panel_paper.png"
target_dataset <- if (length(args) >= 3) tolower(args[3]) else "tpch"
target_bound <- if (length(args) >= 4) as.numeric(args[4]) else 1.0

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
needed <- c("dataset", "query", "mode", "release", "success", "bound_multiplier", "dp_sass_m", "delta", "median_error_pct")
missing <- setdiff(needed, colnames(raw))
if (length(missing) > 0) {
  stop("Missing expected columns: ", paste(missing, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

format_delta <- function(x) {
  formatC(as.numeric(x), format = "e", digits = 0)
}

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
release_levels <- c("average")
release_labels <- c("average" = "SAA average")
delta_levels <- c("1e-08", "1e-07", "1e-06", "1e-05", "1e-04")
delta_colors <- c(
  "1e-08" = "#3b4cc0",
  "1e-07" = "#2c7fb8",
  "1e-06" = "#41ab5d",
  "1e-05" = "#fdae61",
  "1e-04" = "#d7191c"
)

plot_data <- raw %>%
  mutate(
    dataset = tolower(as.character(dataset)),
    query = toupper(as.character(query)),
    success = success_flag(success),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    delta = suppressWarnings(as.numeric(delta)),
    delta_label = format_delta(delta),
    release = tolower(as.character(release)),
    median_error_pct = suppressWarnings(as.numeric(median_error_pct))
  ) %>%
  filter(
    dataset == target_dataset,
    mode == "dp_sass",
    success,
    query %in% query_levels,
    release %in% release_levels,
    abs(bound_multiplier - target_bound) <= max(1e-12, abs(target_bound) * 1e-8),
    dp_sass_m %in% c(64L, 128L, 256L, 512L),
    delta_label %in% delta_levels,
    !is.na(median_error_pct),
    is.finite(median_error_pct)
  ) %>%
  group_by(query, release, dp_sass_m, delta_label) %>%
  summarize(
    median_error_pct = median(median_error_pct, na.rm = TRUE),
    rows = n(),
    .groups = "drop"
  ) %>%
  mutate(
    query = factor(query, levels = query_levels),
    release = factor(release, levels = release_levels, labels = release_labels[release_levels]),
    delta_label = factor(delta_label, levels = delta_levels),
    m_label = factor(paste0("m=", dp_sass_m), levels = paste0("m=", c(64, 128, 256, 512))),
    plot_error = pmax(median_error_pct, 0.001)
  )

if (nrow(plot_data) == 0) {
  stop("No DP-SASS rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

base_theme <- theme_bw(base_size = 20, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.75),
    panel.grid.major = element_line(linewidth = 0.45),
    panel.grid.minor = element_blank(),
    strip.background = element_rect(fill = "grey94", color = "grey55", linewidth = 0.7),
    strip.text = element_text(size = 16, face = "bold", margin = margin(3, 2, 3, 2)),
    axis.text.x = element_text(size = 12, angle = 35, hjust = 1, vjust = 1),
    axis.text.y = element_text(size = 13),
    axis.title = element_text(size = 18),
    legend.position = "top",
    legend.title = element_text(size = 15),
    legend.text = element_text(size = 13),
    legend.key.width = unit(1.1, "cm"),
    legend.key.height = unit(0.45, "cm"),
    plot.margin = margin(6, 8, 6, 8)
  )

p <- ggplot(plot_data, aes(x = m_label, y = plot_error, group = delta_label, color = delta_label)) +
  geom_line(linewidth = 0.8, alpha = 0.9) +
  geom_point(size = 1.9, alpha = 0.95) +
  facet_wrap(~ query, nrow = 1) +
  scale_color_manual(values = delta_colors, drop = FALSE) +
  scale_y_log10(
    labels = label_number(accuracy = 0.01),
    breaks = c(0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000)
  ) +
  labs(x = NULL, y = "Median error vs. full answer (%)", color = expression(delta)) +
  coord_cartesian(clip = "off") +
  base_theme

ggsave(output_png, p, width = 9.4, height = 2.8, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Summary saved to: ", summary_csv)
