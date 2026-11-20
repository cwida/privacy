#!/usr/bin/env Rscript
# Paper-style SF30 timings. Missing modes remain missing; OOM is not a runtime.
script_file <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "systemfonts"))
library(ggplot2)
args <- commandArgs(trailingOnly = TRUE)
if (length(args) != 2) stop("Usage: Rscript plot_tpcds_results.R RUN_DIR OUTPUT_DIR")
run_dir <- args[1]
output_dir <- args[2]
dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
# RUN_DIR is either an archived run (sf30.json beside sf30/summary.csv) or the out_dir of run.py.
config_path <- file.path(run_dir, "sf30.json")
summary_path <- file.path(run_dir, "sf30", "summary.csv")
if (!file.exists(config_path)) {
  config_path <- file.path(run_dir, "run_config.json")
  summary_path <- file.path(run_dir, "summary.csv")
}
config <- readLines(config_path)
if (!any(grepl('"sf": 30', config, fixed = TRUE))) stop("Expected the SF30 run.")
rows <- read.csv(summary_path, stringsAsFactors = FALSE)
as_rows <- rows[rows$mode == "simd_asa_handwritten" & rows$status != "not_available" & rows$query != "q17", ]
queries <- sort(as_rows$query)
if (length(queries) != 68 || anyDuplicated(queries)) stop("Expected 68 executed AS query variants; Q17 requires unsupported STDDEV.")
mode_labels <- c(duckdb = "DuckDB", naive_as = "Naive-SA", simd_asa_handwritten = "SIMD-ASA")
bars <- rows[rows$query %in% queries & rows$mode %in% names(mode_labels), ]
bars$series <- factor(unname(mode_labels[bars$mode]), levels = unname(mode_labels))
bars$oom <- bars$status != "executed_not_validated" & grepl("out of memory", bars$error, ignore.case = TRUE)
bars$measured <- bars$status == "executed_not_validated" & bars$measured_runs == 5 &
  is.finite(bars$median_ms) & bars$median_ms > 0
if (any(!bars$measured & !bars$oom & !(bars$query == "q78" & bars$mode == "naive_as"))) {
  stop("Unexpected missing or failed measurement.")
}
# Current run: Q78 completes at the benchmark memory limit; Q17 is unsupported.
# Retain all measured values from this run without borrowing diagnostic timings.
write.csv(bars, file.path(output_dir, "tpcds_sf30_plot_data.csv"), row.names = FALSE)
as_data <- bars[bars$mode == "simd_asa_handwritten", ]
base <- bars[bars$mode == "duckdb", c("query", "median_ms")]
names(base)[2] <- "baseline_ms"
plot_data <- merge(as_data, base, by = "query")
plot_data$x <- match(plot_data$query, queries)
plot_data$slowdown <- plot_data$median_ms / plot_data$baseline_ms
geomean <- exp(mean(log(plot_data$slowdown[plot_data$measured])))
upper <- max(plot_data$slowdown[plot_data$measured]) * 1.5
lower <- min(0.8, min(plot_data$slowdown[plot_data$measured]) * 0.9)
plot_data$height <- ifelse(plot_data$oom, upper, plot_data$slowdown)
labels <- plot_data[plot_data$measured & plot_data$slowdown >= 2, ]
labels$label <- sprintf("%.1fx", labels$slowdown)
labels$label_y <- pmin(labels$slowdown * rep(c(1.10, 1.46), length.out = nrow(labels)), upper / 1.16)
labels$hjust <- ifelse(labels$query == tail(queries, 1), 1, 0.5)
font <- PaperFont()
p <- ggplot(plot_data, aes(x = x)) +
  geom_rect(aes(xmin = x - 0.36, xmax = x + 0.36, ymin = lower, ymax = height),
            fill = "#4dff4d", color = "#2f7d32", linewidth = 0.16) +
  geom_hline(yintercept = 1, linewidth = 0.65, color = "#4d4d4d") +
  geom_hline(yintercept = geomean, linewidth = 0.6, linetype = "dashed", color = "#2f7d32") +
  geom_text(data = labels, aes(y = label_y, label = label, hjust = hjust),
            size = 3.3, vjust = 0, fontface = "bold", family = font) +
  geom_text(data = plot_data[plot_data$oom, ], aes(y = upper / 1.08, label = "OOM"),
            angle = 90, hjust = 1, size = 3.5, fontface = "bold", family = font) +
  annotate("text", x = Inf, y = upper / 1.06,
           label = sprintf("geomean %.2fx", geomean), hjust = 1.02, vjust = 1,
           size = 4.2, fontface = "bold", family = font, color = "#2f7d32") +
  scale_x_continuous(breaks = seq_along(queries), labels = toupper(queries),
                     expand = expansion(add = 0.45)) +
  scale_y_log10(breaks = c(0.5, 1, 2, 4, 8, 16), labels = function(x) paste0(x, "x"),
                expand = expansion(mult = 0)) +
  coord_cartesian(ylim = c(lower, upper)) +
  labs(x = NULL, y = NULL) +
  theme_bw(base_size = 38, base_family = font) +
  theme(panel.border = element_rect(linewidth = 0.7),
        panel.grid.major.y = element_line(linewidth = 0.5),
        panel.grid.major.x = element_blank(), panel.grid.minor = element_blank(),
        axis.text.x = element_text(angle = 90, hjust = 1, vjust = 0.5, size = 10,
                                   margin = margin(t = 2)),
        axis.text.y = element_text(size = 14.5, margin = margin(r = 1)),
        axis.ticks = element_line(linewidth = 0.35),
        axis.ticks.length = grid::unit(0.7, "mm"), plot.margin = margin(2, 3, 2, 2))
prefix <- file.path(output_dir, "tpcds_sf30_graviton_paper")
ggsave(paste0(prefix, ".png"), p, width = 3200 / 350, height = 720 / 350, dpi = 600)
ggsave(paste0(prefix, ".pdf"), p, width = 3200 / 350, height = 720 / 350, device = cairo_pdf)
saveRDS(p, paste0(prefix, ".rds"))
write.csv(plot_data, file.path(output_dir, "tpcds_sf30_slowdown.csv"), row.names = FALSE)
message("Saved ", prefix, ".png; SIMD-AS runtime / ordinary DuckDB runtime.")
