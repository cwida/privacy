#!/usr/bin/env Rscript
# Compact paper-style DP-SASS sampled-target utility plot at 1x bounds.

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
  stop("Usage: Rscript plot_sass_sampled_m_1x.R results.csv [output_png] [dataset=tpch] [delta=1e-6]")
}

input_csv <- args[1]
output_png <- if (length(args) >= 2) args[2] else "benchmark/dp/tpch_sass_sampled_m_1x_paper.png"
target_dataset <- if (length(args) >= 3) tolower(args[3]) else "tpch"
target_delta <- if (length(args) >= 4) as.numeric(args[4]) else 1e-6

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
needed <- c(
  "dataset", "query", "mode", "release", "success", "bound_multiplier", "dp_sass_m", "delta",
  "saa_estimator_median_error_pct"
)
missing <- setdiff(needed, colnames(raw))
if (length(missing) > 0) {
  stop("Missing expected columns: ", paste(missing, collapse = ", "))
}

success_flag <- function(x) {
  tolower(as.character(x)) %in% c("true", "t", "1")
}

query_levels <- c("Q01", "Q05", "Q06", "Q14", "Q19")
m_levels <- c(64L, 128L, 256L, 512L)

plot_data <- raw %>%
  mutate(
    dataset = tolower(as.character(dataset)),
    query = toupper(as.character(query)),
    success = success_flag(success),
    bound_multiplier = suppressWarnings(as.numeric(bound_multiplier)),
    dp_sass_m = suppressWarnings(as.integer(dp_sass_m)),
    delta = suppressWarnings(as.numeric(delta)),
    release = tolower(as.character(release)),
    sampled_error = suppressWarnings(as.numeric(saa_estimator_median_error_pct))
  ) %>%
  filter(
    dataset == target_dataset,
    mode == "dp_sass",
    release == "average",
    success,
    query %in% query_levels,
    dp_sass_m %in% m_levels,
    abs(bound_multiplier - 1.0) <= 1e-12,
    abs(delta - target_delta) <= max(1e-12, abs(target_delta) * 1e-8),
    !is.na(sampled_error),
    is.finite(sampled_error)
  ) %>%
  group_by(query, dp_sass_m) %>%
  summarize(
    sampled_error = median(sampled_error, na.rm = TRUE),
    rows = n(),
    .groups = "drop"
  ) %>%
  mutate(
    query = factor(query, levels = query_levels),
    m_label = factor(paste0("m=", dp_sass_m), levels = paste0("m=", m_levels)),
    plot_error = pmax(sampled_error, 0.001)
  )

if (nrow(plot_data) == 0) {
  stop("No DP-SASS average rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

base_theme <- theme_bw(base_size = 23, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 0.75),
    panel.grid.major = element_line(linewidth = 0.45),
    panel.grid.minor = element_blank(),
    strip.background = element_rect(fill = "grey94", color = "grey55", linewidth = 0.7),
    strip.text = element_text(size = 17, face = "bold", margin = margin(2, 2, 2, 2)),
    axis.text.x = element_text(size = 15),
    axis.text.y = element_text(size = 15),
    axis.title = element_text(size = 19),
    axis.title.y = element_text(size = 19, hjust = 0.62, margin = margin(r = 7)),
    legend.position = "none",
    plot.margin = margin(3, 7, 3, 9)
  )

p <- ggplot(plot_data, aes(x = m_label, y = plot_error, group = 1)) +
  geom_line(linewidth = 0.9, color = "#009900", alpha = 0.9) +
  geom_point(size = 2.4, color = "#009900", alpha = 0.95) +
  facet_wrap(~ query, nrow = 1) +
  scale_x_discrete(labels = c("m=64" = "64", "m=128" = "128", "m=256" = "256", "m=512" = "512")) +
  scale_y_log10(
    labels = label_number(accuracy = 0.01),
    breaks = c(0.001, 0.01, 0.1, 1, 10, 100)
  ) +
  labs(x = "SAA subsamples (m)", y = "Median error (%)") +
  coord_cartesian(clip = "off") +
  base_theme

ggsave(output_png, p, width = 9.4, height = 2.1, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Summary saved to: ", summary_csv)
