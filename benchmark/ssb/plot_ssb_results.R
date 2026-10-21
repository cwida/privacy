#!/usr/bin/env Rscript
# WIP: prepare SSB inputs for the paper's existing TPC-H runtime plotter.
script_file <- sub("^--file=", "", grep("^--file=", commandArgs(FALSE), value = TRUE)[1])
plotter <- file.path(dirname(dirname(normalizePath(script_file))), "tpch", "plot_tpch_results.R")

args <- commandArgs(trailingOnly = TRUE)
if (length(args) < 3 || length(args) > 4) {
  stop("Usage: Rscript plot_ssb_results.R RESULTS_DIR SCALE_FACTOR THREADS [OUTPUT_DIR]")
}
results_dir <- args[1]
sf <- as.integer(args[2])
threads <- as.integer(args[3])
if (is.na(sf) || sf < 1 || is.na(threads) || threads < 1) stop("Positive SF and threads required.")
output_dir <- if (length(args) == 4) args[4] else results_dir
dir.create(output_dir, recursive = TRUE, showWarnings = FALSE)
queries <- c("q11", "q12", "q13", "q21", "q22", "q23", "q31", "q32", "q33", "q34", "q41", "q42", "q43")
kernels <- c("approximate", "exact")
modes <- c("duckdb_full", "repeated_sql", "asa")
raw <- do.call(rbind, lapply(kernels, function(kernel) {
  path <- file.path(results_dir, kernel, paste0("threads_", threads, "_timings.csv"))
  rows <- read.csv(path, stringsAsFactors = FALSE)
  required <- c("query", "mode", "sum_kernel", "membership_mode", "threads", "memory_limit", "run", "query_ms",
                "max_relative_error", "mean_relative_error", "compared_values", "rounded_mismatches")
  if (!all(required %in% names(rows))) stop("Missing columns in ", path)
  if (any(rows$membership_mode != "query_time_priv_hash")) stop("Expected query-time membership timings: ", path)
  if (any(rows$sum_kernel != kernel) || any(rows$threads != threads)) stop("Configuration mismatch in ", path)
  if (any(!is.finite(rows$query_ms) | rows$query_ms <= 0)) stop("Invalid runtime in ", path)
  expected <- as.vector(outer(queries, modes, paste, sep = ":"))
  observed <- paste(rows$query, rows$mode, sep = ":")
  as_only <- kernel == "exact" && setequal(observed, paste(queries, "asa", sep = ":"))
  if (!as_only && !setequal(observed, expected)) stop("Incomplete query/mode coverage in ", path)
  if (anyDuplicated(rows[c("query", "mode", "run")])) stop("Duplicate measurements in ", path)
  rows
}))
if (length(unique(raw$memory_limit)) != 1) stop("Mixed memory limits.")
if (any(raw$sum_kernel == "exact" & raw$mode == "asa" & raw$rounded_mismatches != 0)) {
  stop("Exact SUM has mismatches beyond output rounding.")
}

times <- aggregate(query_ms ~ query + mode + sum_kernel, raw, median)
counts <- aggregate(run ~ query + mode + sum_kernel, raw, length)
if (length(unique(counts$run)) != 1) stop("Unequal repetition counts.")
repetitions <- counts$run[1]
# Exact runs may measure only AS. Reuse ordinary SQL medians for plotting and
# ratios without writing them back as new measurements in the timing CSV.
if (!any(times$sum_kernel == "exact" & times$mode != "asa")) {
  references <- times[times$sum_kernel == "approximate" & times$mode != "asa", ]
  references$sum_kernel <- "exact"
  times <- rbind(times, references)
}
asa <- times[times$mode == "asa", c("query", "sum_kernel", "query_ms")]
names(asa)[3] <- "asa_ms"
full <- times[times$mode == "duckdb_full", c("query", "sum_kernel", "query_ms")]
names(full)[3] <- "duckdb_ms"
naive <- times[times$mode == "repeated_sql", c("query", "sum_kernel", "query_ms")]
names(naive)[3] <- "naive_ms"
summary <- merge(merge(asa, full), naive)
summary$overhead_vs_full <- summary$asa_ms / summary$duckdb_ms
summary$speedup_vs_repeated <- summary$naive_ms / summary$asa_ms
summary$repetitions <- repetitions

prefix <- file.path(output_dir, sprintf("ssb_sf%d_t%d_preliminary", sf, threads))
write.csv(summary[order(match(summary$sum_kernel, kernels), match(summary$query, queries)), ],
          paste0(prefix, "_summary.csv"), row.names = FALSE)
mode_labels <- c(duckdb_full = "baseline", repeated_sql = "Naive-AS", asa = "SIMD AS")
plots <- list()
for (kernel in kernels) {
  kernel_dir <- file.path(output_dir, kernel)
  dir.create(kernel_dir, recursive = TRUE, showWarnings = FALSE)
  rows <- times[times$sum_kernel == kernel, ]
  input <- data.frame(query = rows$query, mode = unname(mode_labels[rows$mode]), median_ms = rows$query_ms)
  input_file <- file.path(kernel_dir, sprintf("ssb_sf%d_paper_input.csv", sf))
  write.csv(input, input_file, row.names = FALSE)
  plot_file <- file.path(kernel_dir, "ssb_paper_plot.rds")
  status <- system2(file.path(R.home("bin"), "Rscript"),
                    shQuote(c(plotter, normalizePath(input_file), normalizePath(kernel_dir), "ssb",
                              file.path(normalizePath(kernel_dir), basename(plot_file)))))
  if (status != 0) stop("Paper plotter failed for ", kernel)
  plots[[kernel]] <- readRDS(plot_file)
}

# Keep one copy of both ordinary SQL measurements, from the first input file.
ordinary <- times[times$mode != "asa" & times$sum_kernel == kernels[1], ]
ordinary$component <- ifelse(ordinary$mode == "duckdb_full", "DuckDB", "Naive-AS")
baseline <- ordinary[ordinary$mode == "duckdb_full", ]
as_bars <- times[times$mode == "asa", ]
as_bars$component <- ifelse(as_bars$sum_kernel == "approximate", "SIMD-AS (approx.)", "SIMD-AS (exact)")
bars <- rbind(ordinary, as_bars)
series <- c("DuckDB", "Naive-AS", "SIMD-AS (approx.)", "SIMD-AS (exact)")
bars$component <- factor(bars$component, levels = series)
bars$x_pos <- match(bars$query, queries) + c(-0.33, -0.11, 0.11, 0.33)[as.integer(bars$component)]
bars$time <- bars$query_ms
labels <- merge(bars[bars$mode != "duckdb_full", ],
                data.frame(query = baseline$query, baseline_ms = baseline$query_ms))
ratios <- labels$query_ms / labels$baseline_ms
labels$label <- ifelse(ratios >= 10, sprintf("%.0fx", ratios), sprintf("%.1fx", ratios))
as_top <- ave(labels$query_ms * (labels$mode == "asa"), labels$query, FUN = max)
labels$y_pos <- ifelse(labels$mode == "repeated_sql", labels$query_ms * 1.15,
                      as_top * ifelse(labels$sum_kernel == "approximate", 1.15, 2.5))

# Reuse the paper renderer's axes, dimensions, fonts, and bar layer.
p <- plots$approximate
p$data <- bars
p$layers <- p$layers[1]
p$mapping$width <- NULL
p$layers[[1]]$aes_params$width <- 0.20
p <- suppressMessages(p + ggplot2::scale_fill_manual(
  values = setNames(c("#95a5a6", "#a8d4ff", "#4dff4d", "#52b788"), series), name = NULL
))
p <- p + ggplot2::theme(legend.text = ggplot2::element_text(size = 23))
p <- suppressMessages(p + ggplot2::scale_x_continuous(
  breaks = seq_along(queries), labels = sub("^q([0-9])([0-9])$", "Q\\1.\\2", queries),
  expand = ggplot2::expansion(add = 0.4)))
p <- p + ggplot2::geom_text(data = labels,
  ggplot2::aes(x = x_pos, y = y_pos, label = label), inherit.aes = FALSE,
  size = 5, vjust = 0, fontface = "bold", family = p$theme$text$family)

# Clip diagonal lines to each exact bar in log-runtime coordinates, without
# requiring a separate pattern package.
stripes <- list()
exact <- bars[bars$sum_kernel == "exact", ]
for (i in seq_len(nrow(exact))) {
  left <- exact$x_pos[i] - 0.10
  right <- exact$x_pos[i] + 0.10
  top <- log10(exact$query_ms[i])
  for (intercept in seq(-0.25, top, by = 0.16)) {
    start <- max(left, left - intercept)
    end <- min(right, left + top - intercept)
    if (start < end) stripes[[length(stripes) + 1]] <- data.frame(
      x = start, xend = end, y = 10^(intercept + start - left), yend = 10^(intercept + end - left))
  }
}
p <- p + ggplot2::geom_segment(data = do.call(rbind, stripes),
  ggplot2::aes(x = x, xend = xend, y = y, yend = yend),
  inherit.aes = FALSE, color = "#245d2c", linewidth = 0.3, alpha = 0.55)
combined <- file.path(output_dir, sprintf("ssb_sf%d_t%d_combined_paper", sf, threads))
ggplot2::ggsave(paste0(combined, ".png"), p, width = 4000 / 350, height = 1500 / 350, dpi = 350)
ggplot2::ggsave(paste0(combined, ".pdf"), p, width = 4000 / 350, height = 1500 / 350, device = cairo_pdf)
message("Combined figure: ", combined, ".{png,pdf}; labels are runtime / full-data DuckDB runtime")
