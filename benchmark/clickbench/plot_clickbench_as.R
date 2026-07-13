#!/usr/bin/env Rscript
# Paper-style ClickBench AS plot. Input CSV is produced by as_clickbench_benchmark:
# query,mode,median_ms or query,mode,m,median_ms with modes "baseline" and "SIMD-AS".

user_lib <- Sys.getenv("R_LIBS_USER")
if (user_lib == "") user_lib <- file.path(Sys.getenv("HOME"), "R", "libs")
if (!dir.exists(user_lib)) dir.create(user_lib, recursive = TRUE, showWarnings = FALSE)
.libPaths(c(user_lib, .libPaths()))

required_packages <- c("ggplot2", "dplyr", "readr", "scales", "stringr", "systemfonts")
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
  library(stringr)
})

base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE))) {
    "Linux Libertine"
  } else {
    "serif"
  }
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
get_script_dir <- function() {
  ca <- commandArgs(trailingOnly = FALSE)
  fa <- grep("^--file=", ca, value = TRUE)
  if (length(fa) > 0) return(dirname(normalizePath(sub("^--file=", "", fa[1]))))
  getwd()
}

script_dir <- get_script_dir()
input_csv <- if (length(args) >= 1) args[1] else file.path(script_dir, "as_clickbench_results.csv")
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
stopifnot(all(c("query", "mode", "median_ms") %in% colnames(raw)))

has_m_column <- "m" %in% colnames(raw)
if (!has_m_column) {
  raw$m <- ifelse(raw$mode == "SIMD-AS", 64L, 0L)
}
has_m_sweep <- raw %>%
  filter(mode == "SIMD-AS") %>%
  pull(m) %>%
  unique() %>%
  length() > 1

paper_colors <- c(
  "DuckDB" = "#95a5a6",
  "SIMD-AS" = "#4dff4d",
  "SIMD-AS m=64" = "#4dff4d",
  "SIMD-AS m=512" = "#2c7fb8"
)
mode_levels <- if (has_m_sweep) {
  c("DuckDB", paste0("SIMD-AS m=", sort(unique(raw$m[raw$mode == "SIMD-AS"]))))
} else {
  c("DuckDB", "SIMD-AS")
}
paper_colors <- paper_colors[mode_levels]

raw <- raw %>%
  mutate(
    query = as.character(query),
    mode = as.character(mode),
    m = as.integer(m),
    median_ms = as.numeric(median_ms),
    component = case_when(
      mode == "baseline" ~ "DuckDB",
      mode == "SIMD-AS" & has_m_sweep ~ paste0("SIMD-AS m=", m),
      mode == "SIMD-AS" ~ "SIMD-AS",
      TRUE ~ NA_character_
    ),
    qnum = as.integer(str_extract(query, "\\d+")),
    failed = median_ms < 0
  ) %>%
  filter(!is.na(component), !is.na(qnum), is.finite(median_ms))

if (nrow(raw) == 0) {
  stop("No plottable rows found in ", input_csv)
}

query_order <- raw %>% arrange(qnum) %>% pull(query) %>% unique()
query_labels <- paste0("Q", as.integer(str_extract(query_order, "\\d+")))

positive_max <- max(raw$median_ms[raw$median_ms > 0], na.rm = TRUE)
fail_y <- positive_max * 1.5

plot_data <- raw %>%
  mutate(
    bar = component,
    component = factor(component, levels = mode_levels),
    qidx = match(query, query_order),
    original_time = median_ms,
    time = case_when(
      failed ~ fail_y,
      median_ms < 1.5 ~ 1.5,
      TRUE ~ median_ms
    )
  )

offsets <- if (length(mode_levels) == 2) {
  setNames(c(-0.2, 0.2), mode_levels)
} else {
  setNames(seq(-0.3, 0.3, length.out = length(mode_levels)), mode_levels)
}
bar_width <- if (length(mode_levels) == 2) 0.35 else 0.24
plot_data <- plot_data %>%
  mutate(x_pos = qidx + offsets[as.character(component)])

baseline_df <- raw %>%
  filter(component == "DuckDB") %>%
  select(query, baseline_time = median_ms)

slowdown_labels <- raw %>%
  filter(component != "DuckDB", !failed) %>%
  inner_join(baseline_df, by = "query") %>%
  filter(baseline_time > 0) %>%
  mutate(
    slowdown = median_ms / baseline_time,
    qidx = match(query, query_order),
    component = factor(component, levels = mode_levels),
    x_pos = qidx + offsets[as.character(component)],
    y_pos = pmax(median_ms, 1.5) * 1.15,
    label = {
      s <- pmax(slowdown, 1.0)
      ifelse(s >= 10, sprintf("%.0fx", s), sprintf("%.1fx", s))
    }
  )
if (has_m_sweep) {
  slowdown_labels <- slowdown_labels %>% filter(slowdown >= 3)
}

failed_bars <- plot_data %>%
  filter(failed) %>%
  mutate(label = "FAILED", y_pos = sqrt(1.5 * fail_y))

y_upper <- max(c(plot_data$time, slowdown_labels$y_pos), na.rm = TRUE) * 1.25
bar_data <- if (has_m_sweep) {
  plot_data %>% filter(!failed)
} else {
  plot_data
}
label_size <- if (has_m_sweep) 3.5 else 5
base_size <- if (has_m_sweep) 30 else 40
legend_text_size <- if (has_m_sweep) 20 else 28
axis_x_size <- if (has_m_sweep) 12 else 24
axis_y_size <- if (has_m_sweep) 18 else 24

p <- ggplot(bar_data, aes(x = x_pos, y = time, fill = component, width = bar_width)) +
  geom_col() +
  geom_text(data = slowdown_labels, aes(x = x_pos, y = y_pos, label = label),
            inherit.aes = FALSE, size = label_size, vjust = 0, fontface = "bold", family = base_font) +
  { if (has_m_sweep && nrow(failed_bars) > 0)
    geom_point(data = failed_bars, aes(x = x_pos, y = fail_y),
               inherit.aes = FALSE, shape = 4, size = 1.6, stroke = 0.45, color = "#b22222")
  } +
  { if (!has_m_sweep && nrow(failed_bars) > 0)
    geom_text(data = failed_bars, aes(x = x_pos, y = y_pos, label = label),
              inherit.aes = FALSE, angle = 90, hjust = 0.5, vjust = 0.5,
              size = 5, fontface = "bold", color = "black", family = base_font)
  } +
  scale_fill_manual(values = paper_colors, name = NULL) +
  scale_x_continuous(breaks = seq_along(query_order), labels = query_labels, expand = expansion(add = 0.4)) +
  scale_y_log10(labels = function(x) ifelse(x >= 100, paste0(x / 1000, "s"), paste0(x, "ms"))) +
  coord_cartesian(ylim = c(NA, y_upper), clip = "off") +
  labs(x = NULL, y = NULL) +
  theme_bw(base_size = base_size, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 1.0),
    panel.grid.minor = element_blank(),
    legend.position = "top",
    legend.title = element_blank(),
    legend.text = element_text(size = legend_text_size),
    legend.margin = margin(0, 0, -5, 0),
    legend.box.margin = margin(0, 0, -20, 0),
    axis.text.x = element_text(angle = 45, hjust = 1, size = axis_x_size),
    axis.text.y = element_text(size = axis_y_size),
    axis.title = element_text(size = 32),
    plot.title = element_blank(),
    plot.margin = margin(2, 5, 5, 5)
  )

out_file <- file.path(output_dir, "clickbench_as_benchmark_plot_paper.png")
png(filename = out_file, width = 4000, height = 1500, res = 350)
print(p)
dev.off()

message("font: ", base_font)
message("queries plotted: ", length(query_order))
message("Plot saved to: ", out_file)
