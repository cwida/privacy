#!/usr/bin/env Rscript
# Median of five warm query executions, using the paper style.
script_file <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "dplyr", "scales"))
suppressPackageStartupMessages(library(ggplot2))

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 1 || length(args) > 2) {
  stop("Usage: Rscript plot_meps_results.R RESULTS_DIR [THREADS]")
}
root <- args[1]
threads <- if (length(args) == 2) as.integer(args[2]) else 8L
if (is.na(threads) || threads < 1) stop("Positive thread count required.")
queries <- list(damico2006 = c("q01_adults", "q02_nonelderly"),
                ahrq2016 = c("q01_office", "q02_office_physician", "q03_outpatient",
                             "q04_outpatient_physician", "q05_visits_per_person"))
modes <- c("duckdb", "naive_as", "conditional_sql", "simd_as")
raw <- list()
for (kernel in c("approximate", "exact")) for (workload in names(queries)) {
  path <- file.path(root, kernel, workload,
                    paste0("threads_", threads, "_timings.csv"))
  rows <- read.csv(path, stringsAsFactors = FALSE)
  required <- c("workload", "query", "mode", "threads", "run", "worlds", "query_ms", "max_normalized_error")
  if (!all(required %in% names(rows))) stop("Missing columns: ", path)
  expected <- expand.grid(query = queries[[workload]], mode = modes, run = 0:5, stringsAsFactors = FALSE)
  key <- function(x) paste(x$query, x$mode, x$run, sep = ":")
  if (anyDuplicated(key(rows)) || !setequal(key(rows), key(expected))) stop("Incomplete suite: ", path)
  if (any(rows$workload != workload | rows$threads != threads)) stop("Configuration mismatch: ", path)
  if (any(!is.finite(rows$query_ms) | rows$query_ms <= 0)) stop("Invalid timing: ", path)
  expected_worlds <- ifelse(rows$mode == "duckdb", 1L, ifelse(workload == "damico2006", 64L, 128L))
  if (any(rows$worlds != expected_worlds)) stop("World-count mismatch: ", path)
  tolerance <- ifelse(rows$mode == "simd_as", 1e-3, 1e-8)
  if (any(!is.finite(rows$max_normalized_error) | rows$max_normalized_error > tolerance)) {
    stop("Failed numerical check: ", path)
  }
  if (!"timing_scope" %in% names(rows) ||
      any(rows$timing_scope != ifelse(rows$mode == "naive_as", "replicate1_x_worlds", "complete_query"))) {
    stop("Expected first-replicate extrapolation for naive timings: ", path)
  }
  rows$kernel <- kernel
  raw[[length(raw) + 1L]] <- rows
}
raw <- do.call(rbind, raw)
write.csv(raw, file.path(root, "meps_five_runs_raw.csv"), row.names = FALSE)
# Run zero warms the database; retain the five measured executions.
warm <- raw[raw$run > 0, ]
warm <- warm[warm$kernel == "approximate" | warm$mode == "simd_as", ]
warm$series <- ifelse(warm$mode == "simd_as",
                     ifelse(warm$kernel == "approximate", "SIMD-AS (approx.)", "SIMD-AS (non-approx.)"),
                     c(duckdb = "DuckDB", naive_as = "Naive-AS", conditional_sql = "Conditional SQL")[warm$mode])
series <- c("DuckDB", "Naive-AS", "Conditional SQL", "SIMD-AS (approx.)", "SIMD-AS (non-approx.)")
summary <- dplyr::summarise(dplyr::group_by(warm, workload, query, series),
                          median_ms = median(query_ms), n = dplyr::n(), .groups = "drop")
if (any(summary$n != 5L)) stop("Expected exactly five measured runtimes per bar.")
write.csv(summary, file.path(root, "meps_five_runs_median.csv"), row.names = FALSE)

# Obtain exactly the paper renderer used by SSB, rather than a separate theme.
plotter <- file.path(dirname(dirname(normalizePath(script_file))), "tpch", "plot_tpch_results.R")
query_order <- unlist(queries, use.names = FALSE)
query_positions <- seq_along(query_order) + ifelse(seq_along(query_order) > 2, 0.4, 0)
separator_position <- mean(query_positions[2:3])
query_labels <- c("Adults", "Nonelderly", "OB", "OB phys.", "OP", "OP phys.", "Visits")
paper_dir <- tempfile("meps-paper-renderer-")
dir.create(paper_dir)
input <- summary[summary$series %in% c("DuckDB", "Naive-AS", "SIMD-AS (approx.)"), ]
input$query <- sprintf("q%02d", match(input$query, query_order))
input$mode <- c("DuckDB" = "baseline", "Naive-AS" = "Naive-AS",
                "SIMD-AS (approx.)" = "SIMD AS")[input$series]
# Use microseconds internally so sub-ms DuckDB bars remain above the log-bar origin.
input$median_ms <- input$median_ms * 1000
input_file <- file.path(paper_dir, "meps_paper_input.csv")
write.csv(input[c("query", "mode", "median_ms")], input_file, row.names = FALSE)
rds_file <- file.path(paper_dir, "paper_plot.rds")
status <- system2(file.path(R.home("bin"), "Rscript"),
                  shQuote(c(plotter, input_file, paper_dir, "ssb", rds_file)),
                  stdout = FALSE)
if (status != 0) stop("Paper renderer failed.")
p <- readRDS(rds_file)

series_labels <- c("DuckDB", "Naive-AS", "Conditional SQL", "SIMD-AS (approx.)", "SIMD-AS (exact)")
summary$component <- factor(summary$series, levels = series, labels = series_labels)
summary$x_pos <- query_positions[match(summary$query, query_order)] +
  c(-0.36, -0.18, 0, 0.18, 0.36)[as.integer(summary$component)]
summary$time <- 1000 * summary$median_ms
baseline <- summary[summary$series == "DuckDB", c("query", "median_ms")]
names(baseline)[2] <- "baseline_ms"
labels <- merge(summary[summary$series != "DuckDB", ], baseline, by = "query")
ratios <- labels$median_ms / labels$baseline_ms
labels$label <- ifelse(ratios >= 10, sprintf("%.0fx", ratios), sprintf("%.1fx", ratios))
as_top <- ave(labels$time * grepl("^SIMD-AS", labels$series), labels$query, FUN = max)
sql_top <- ave(labels$time * (labels$series %in% c("Naive-AS", "Conditional SQL")), labels$query, FUN = max)
labels$y_pos <- ifelse(labels$series == "Naive-AS", labels$time * 1.15,
                      ifelse(labels$series == "Conditional SQL", sql_top * 3.3,
                             as_top * ifelse(labels$series == "SIMD-AS (approx.)", 1.15, 2.5)))

# Use the registered serif family on both native PDF and Quartz (Times on this Mac).
# Arbitrary installed font names are not registered with those graphics devices.
base_font <- "serif"
for (element_name in names(p$theme)) {
  if (inherits(p$theme[[element_name]], "element_text")) p$theme[[element_name]]$family <- base_font
}
p$data <- summary
p$layers <- p$layers[1]
p$mapping$width <- NULL
p$layers[[1]]$aes_params$width <- 0.16
p$layers[[1]]$geom <- ggproto(NULL, GeomCol,
  draw_key = function(data, params, size) {
    key <- draw_key_rect(data, params, size)
    if (!identical(data$fill, "#52b788")) return(key)
    grid::grobTree(key, grid::segmentsGrob(
      x0 = c(0, 0, 0.2, 0.6), y0 = c(0.2, 0.6, 0, 0),
      x1 = c(0.8, 0.4, 1, 1), y1 = c(1, 1, 0.8, 0.4),
      gp = grid::gpar(col = "#245d2c", lwd = 0.65)))
  })
p <- suppressMessages(p + scale_fill_manual(
  values = setNames(c("#95a5a6", "#a8d4ff", "#f0b35a", "#4dff4d", "#52b788"), series_labels),
  name = NULL))
p <- suppressMessages(p + scale_x_continuous(
  breaks = query_positions, labels = query_labels, expand = expansion(add = 0.1),
  sec.axis = dup_axis(breaks = c(mean(query_positions[1:2]), mean(query_positions[3:7])),
                      labels = c("MEPS 2006 (64 replicates)", "MEPS 2016 (128 replicates)"), name = NULL)))
p <- suppressMessages(p + scale_y_log10(
  labels = function(x) ifelse(x >= 1e6, paste0(x / 1e6, "s"), paste0(x / 1000, "ms"))))
p <- p + coord_cartesian(ylim = c(100, max(labels$y_pos) * 1.3), clip = "on") +
  theme(text = element_text(family = base_font),
        axis.text = element_text(family = base_font),
        axis.text.x.top = element_text(size = 16, angle = 0, hjust = 0.5, color = "black",
                                       margin = margin(b = 4)),
        axis.ticks.x.top = element_blank(),
        legend.text = element_text(size = 18.5, family = base_font, margin = margin(l = 8, r = 8)),
        legend.key.width = grid::unit(0.4, "cm"), legend.key.height = grid::unit(0.4, "cm"),
        legend.spacing.x = grid::unit(0.12, "cm"),
        legend.justification = "left", legend.box.just = "left", legend.location = "plot",
        legend.box.spacing = grid::unit(9, "pt"),
        legend.margin = margin(0, 0, 0, 0), legend.box.margin = margin(0, 0, 0, 12),
        plot.margin = margin(12, 10, 12, 10)) +
  geom_text(data = labels, aes(x = x_pos, y = y_pos, label = label), inherit.aes = FALSE,
            size = 5, vjust = 0, fontface = "bold", family = base_font) +
  geom_vline(xintercept = separator_position, linetype = "dashed", color = "grey45", linewidth = 0.4)

# Match the diagonal striping on SSB's exact-SUM bars.
stripes <- list()
exact <- summary[summary$series == "SIMD-AS (non-approx.)", ]
for (i in seq_len(nrow(exact))) {
  left <- exact$x_pos[i] - 0.08
  right <- exact$x_pos[i] + 0.08
  bottom <- 2 # 100 microseconds: the visible lower limit.
  top <- log10(exact$time[i])
  for (intercept in seq(bottom - 0.16, top, by = 0.16)) {
    start <- max(left, left + bottom - intercept)
    end <- min(right, left + top - intercept)
    if (start < end) stripes[[length(stripes) + 1L]] <- data.frame(
      x = start, xend = end, y = 10^(intercept + start - left), yend = 10^(intercept + end - left))
  }
}
p <- p + geom_segment(data = do.call(rbind, stripes),
                      aes(x = x, xend = xend, y = y, yend = yend), inherit.aes = FALSE,
                      color = "#245d2c", linewidth = 0.3, alpha = 0.55)
prefix <- file.path(root, paste0("meps_t", threads, "_five_runs_median"))
ggsave(paste0(prefix, ".png"), p, width = 4000 / 350, height = 1750 / 350, dpi = 350,
       type = if (capabilities("aqua")) "quartz" else "cairo")
ggsave(paste0(prefix, ".pdf"), p, width = 4000 / 350, height = 1750 / 350, device = "pdf")
message("Saved ", prefix, ".{png,pdf}; medians of five warm executions; labels = runtime / DuckDB.")
