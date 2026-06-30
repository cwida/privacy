#!/usr/bin/env Rscript
# ClickBench DP benchmark plotter — styled after Figure 1 of the SIMD-PAC paper,
# but for the DP sample-and-aggregate mechanism (baseline vs naive vs optimized).
# Same palette/font/theme as plot_clickbench_results.R, with PAC swapped for DP:
#   baseline -> DuckDB (gray #95a5a6),  naive_dp -> "naive DP" (PAC-DB blue #a8d4ff),
#   dp_sass  -> "SIMD-DP" (bright green #4dff4d).
# Usage: Rscript plot_clickbench_dp.R path/to/clickbench_full_dpsass_results.csv [output_dir]

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
  library(ggplot2); library(dplyr); library(readr); library(scales); library(stringr)
})

# Font fallback: Linux Libertine if available, otherwise serif (same as the PAC plot script).
base_font <- tryCatch({
  if (any(grepl("Linux Libertine", systemfonts::system_fonts()$family, fixed = TRUE)))
    "Linux Libertine" else "serif"
}, error = function(e) "serif")

args <- commandArgs(trailingOnly = TRUE)
get_script_dir <- function() {
  ca <- commandArgs(trailingOnly = FALSE)
  fa <- grep("^--file=", ca, value = TRUE)
  if (length(fa) > 0) return(dirname(normalizePath(sub("^--file=", "", fa[1]))))
  getwd()
}
script_dir <- get_script_dir()
input_csv <- if (length(args) >= 1) args[1] else file.path(script_dir, "clickbench_full_dpsass_results.csv")
output_dir <- if (length(args) >= 2) args[2] else dirname(input_csv)
if (!dir.exists(output_dir)) dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)

raw <- suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
stopifnot(all(c("query", "mode", "run", "time_ms") %in% colnames(raw)))
if (!"success" %in% colnames(raw)) raw$success <- TRUE
raw <- raw %>%
  mutate(query = as.character(query), mode = as.character(mode),
         success = as.logical(success)) %>%
  filter(!is.na(time_ms) & !is.na(mode))

# Display labels + ordering + colours (paper palette, PAC -> DP).
label_map <- c(baseline = "DuckDB", naive_dp = "naive DP", dp_sass = "SIMD-DP")
mode_levels <- c("DuckDB", "naive DP", "SIMD-DP")
paper_colors <- c("DuckDB" = "#95a5a6", "naive DP" = "#a8d4ff", "SIMD-DP" = "#4dff4d")

raw <- raw %>% filter(mode %in% names(label_map)) %>%
  mutate(disp = factor(unname(label_map[mode]), levels = mode_levels))

# The comparable set = queries that HAVE a naive DP variant (== the dp_sass-privatizable set);
# this excludes non-aggregate passthroughs (e.g. Q25-27) that dp_sass runs as a no-op.
inset <- raw %>% filter(mode == "naive_dp") %>% pull(query) %>% unique()
raw <- raw %>% filter(query %in% inset)

# Per-(query,mode) mean of successful runs.
summ <- raw %>% group_by(query, disp) %>%
  summarize(mean_time = mean(time_ms[success], na.rm = TRUE),
            any_ok = any(success), .groups = "drop") %>%
  filter(any_ok & is.finite(mean_time))

summ <- summ %>% mutate(qnum = as.integer(str_extract(query, "\\d+")))
qorder <- summ %>% arrange(qnum) %>% pull(query) %>% unique()
summ$query <- factor(summ$query, levels = qorder)
summ <- summ %>% mutate(mean_time_plot = ifelse(mean_time < 1.5, 1.5, mean_time))

# Slowdown labels: SIMD-DP / DuckDB, above the green bars (as in Figure 1).
base_t <- summ %>% filter(disp == "DuckDB")  %>% select(query, b = mean_time_plot)
sass_t <- summ %>% filter(disp == "SIMD-DP") %>% select(query, s = mean_time_plot)
slow <- inner_join(sass_t, base_t, by = "query") %>%
  mutate(label = sprintf("%.1fx", s / b), y_pos = s * 1.15)

# naive DP failures (e.g. OOM at full scale): in-set queries with no successful naive bar.
# Render them as full-height naive-DP bars (matching the failure bars in the other plots),
# with a vertical, centred "OOM" label on the bar.
ok_naive <- summ %>% filter(disp == "naive DP") %>% pull(query) %>% as.character()
oom_q <- setdiff(as.character(qorder), ok_naive)
# Full-height failure bars: above the tallest real bar; with top expansion removed (below)
# this makes the OOM bars reach the very top of the panel frame.
ymax_fill <- max(summ$mean_time_plot) * 1.7
oom_bars <- NULL
oom_lab <- NULL
if (length(oom_q) > 0) {
  oom_bars <- tibble(
    query = factor(oom_q, levels = qorder),
    disp = factor("naive DP", levels = mode_levels),
    mean_time = ymax_fill, any_ok = TRUE,
    qnum = as.integer(str_extract(oom_q, "\\d+")),
    mean_time_plot = ymax_fill
  )
  oom_lab <- tibble(
    query = factor(oom_q, levels = qorder),
    y_pos = sqrt(1.5 * ymax_fill),  # geometric centre of the bar on the log axis
    label = "OOM"
  )
}
plot_df <- bind_rows(summ, oom_bars)

base_size <- 40
p <- ggplot(plot_df, aes(x = query, y = mean_time_plot, fill = disp)) +
  geom_col(position = position_dodge2(width = 0.8, preserve = "single"), width = 0.7,
           colour = "black", linewidth = 0.25) +
  geom_text(data = slow, aes(x = query, y = y_pos, label = label), inherit.aes = FALSE,
            size = 5, vjust = 0, fontface = "bold", nudge_x = 0.22, family = base_font) +
  { if (!is.null(oom_lab))
      geom_text(data = oom_lab, aes(x = query, y = y_pos, label = label), inherit.aes = FALSE,
                size = 5, angle = 90, hjust = 0.5, vjust = 0.5, fontface = "bold",
                colour = "black", family = base_font) } +
  scale_fill_manual(values = paper_colors, name = NULL) +
  scale_x_discrete(labels = function(x) paste0("Q", x), expand = expansion(add = 0.8)) +
  scale_y_log10(labels = function(x) ifelse(x >= 100, paste0(x / 1000, "s"), paste0(x, "ms")),
                expand = expansion(mult = c(0, 0))) +
  labs(x = NULL, y = NULL) +
  theme_bw(base_size = base_size, base_family = base_font) +
  theme(
    panel.border = element_rect(linewidth = 1.0),
    panel.grid.major = element_line(linewidth = 1.0),
    panel.grid.minor = element_blank(),
    legend.position = "top", legend.title = element_blank(),
    legend.text = element_text(size = 28),
    legend.margin = margin(0, 0, -5, 0), legend.box.margin = margin(0, 0, -20, 0),
    axis.text.x = element_text(angle = 45, hjust = 1, size = 24),
    axis.text.y = element_text(size = 24), plot.margin = margin(2, 5, 5, 5)
  )

out_file <- file.path(output_dir, "clickbench_dpsass_plot_paper.png")
png(filename = out_file, width = 4000, height = 1400, res = 350)
print(p); dev.off()
message("font: ", base_font)
message("queries plotted: ", length(qorder), " | naive DP failures: ",
        if (length(oom_q)) paste(oom_q, collapse = ", ") else "none")
message("Plot saved to: ", out_file)
