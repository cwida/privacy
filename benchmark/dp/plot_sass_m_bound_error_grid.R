#!/usr/bin/env Rscript
# Paper-style DP-SASS bound/m grid with full-answer and sampled-target errors.

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
  stop("Usage: Rscript plot_sass_m_bound_error_grid.R results.csv [output_png] [dataset=tpch] [delta=1e-6]")
}

input_csv <- args[1]
output_png <- if (length(args) >= 2) args[2] else "benchmark/dp/tpch_sass_m_bound_error_grid_paper.png"
target_dataset <- if (length(args) >= 3) tolower(args[3]) else "tpch"
target_delta <- if (length(args) >= 4) as.numeric(args[4]) else 1e-6

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
needed <- c(
  "dataset", "query", "mode", "release", "success", "bound_multiplier", "dp_sass_m", "delta",
  "median_error_pct", "saa_estimator_median_error_pct"
)
missing <- setdiff(needed, colnames(raw))
if (length(missing) > 0) {
  stop("Missing expected columns: ", paste(missing, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
bound_levels <- c(0.01, 0.1, 1, 10, 100)
bound_labels <- c("0.01x", "0.1x", "1x", "10x", "100x")
m_levels <- c(64L, 128L, 256L, 512L)
m_colors <- c(
  "m=64" = "#66bd63",
  "m=128" = "#1b9e77",
  "m=256" = "#238b45",
  "m=512" = "#00441b"
)
metric_levels <- c("Full", "Sampled")

filtered <- raw %>%
  mutate(
    dataset = tolower(as.character(dataset)),
    query = toupper(as.character(query)),
    success = success_flag(success),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    delta = suppressWarnings(as.numeric(delta)),
    release = tolower(as.character(release)),
    full_error = suppressWarnings(as.numeric(median_error_pct)),
    sampled_error = suppressWarnings(as.numeric(saa_estimator_median_error_pct))
  ) %>%
  filter(
    dataset == target_dataset,
    mode == "dp_sass",
    release == "average",
    success,
    query %in% query_levels,
    dp_sass_m %in% m_levels,
    bound_multiplier %in% bound_levels,
    abs(delta - target_delta) <= max(1e-12, abs(target_delta) * 1e-8)
  )

plot_data <- bind_rows(
  filtered %>%
    transmute(query, bound_multiplier, dp_sass_m, metric = "Full", error_pct = full_error),
  filtered %>%
    transmute(query, bound_multiplier, dp_sass_m, metric = "Sampled", error_pct = sampled_error)
) %>%
  filter(!is.na(error_pct), is.finite(error_pct)) %>%
  group_by(query, metric, bound_multiplier, dp_sass_m) %>%
  summarize(
    error_pct = median(error_pct, na.rm = TRUE),
    rows = n(),
    .groups = "drop"
  ) %>%
  mutate(
    query = factor(query, levels = query_levels),
    metric = factor(metric, levels = metric_levels),
    bound_label = factor(bound_labels[match(bound_multiplier, bound_levels)], levels = bound_labels),
    m_label = factor(paste0("m=", dp_sass_m), levels = paste0("m=", m_levels)),
    plot_error = pmax(error_pct, 0.001)
  )

if (nrow(plot_data) == 0) {
  stop("No DP-SASS average rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

base_theme <- theme_bw(base_size = 22, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.75),
    panel.grid.major = element_line(linewidth = 0.45),
    panel.grid.minor = element_blank(),
    strip.background = element_rect(fill = "grey94", color = "grey55", linewidth = 0.7),
    strip.text.x = element_text(size = 15, face = "bold", margin = margin(3, 2, 3, 2)),
    strip.text.y.left = element_text(size = 14, face = "bold", margin = margin(2, 3, 2, 3)),
    strip.placement = "outside",
    axis.text.x = element_text(size = 10, angle = 35, hjust = 1, vjust = 1),
    axis.text.y = element_text(size = 11),
    axis.title = element_text(size = 18),
    legend.position = "top",
    legend.title = element_text(size = 15),
    legend.text = element_text(size = 13),
    legend.key.width = unit(1.15, "cm"),
    legend.key.height = unit(0.45, "cm"),
    plot.margin = margin(6, 8, 6, 8)
  )

p <- ggplot(plot_data, aes(x = bound_label, y = plot_error, group = m_label, color = m_label)) +
  geom_line(linewidth = 0.8, alpha = 0.9) +
  geom_point(size = 1.8, alpha = 0.95) +
  facet_grid(metric ~ query, scales = "free_y", switch = "y") +
  scale_color_manual(values = m_colors, drop = FALSE) +
  scale_y_log10(
    labels = label_number(accuracy = 0.01),
    breaks = c(0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000)
  ) +
  labs(x = "Output-bound multiplier", y = "Median error (%)", color = "SAA samples") +
  coord_cartesian(clip = "off") +
  base_theme

ggsave(output_png, p, width = 9.4, height = 4.6, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Summary saved to: ", summary_csv)
