#!/usr/bin/env Rscript
# Plot Q01 lane stability for AVG-only and SUM/COUNT variants.

script_file <- sub("^--file=", "", grep("^--file=", commandArgs(trailingOnly = FALSE), value = TRUE)[1])
source(file.path(dirname(dirname(normalizePath(script_file))), "plot_common.R"))
RequirePlotPackages(c("ggplot2", "dplyr", "readr", "scales", "systemfonts"))

suppressPackageStartupMessages({
	library(ggplot2)
	library(dplyr)
	library(readr)
	library(scales)
})

base_font <- PaperFont()

args <- commandArgs(trailingOnly = TRUE)
if (!(length(args) %in% c(2, 3))) {
	stop("Usage: Rscript plot_q01_stability.R avg.csv [sum_count.csv] output.png")
}
input_csvs <- if (length(args) == 2) args[1] else args[1:2]
output_png <- args[length(args)]

success_flag <- function(x) {
	tolower(as.character(x)) %in% c("true", "t", "1")
}

query_labels <- c(
	"q01_avg_qty_all" = "AVG(qty)",
	"q01_avg_price_all" = "AVG(price)",
	"q01_avg_disc_all" = "AVG(disc)",
	"q01_avg_all" = "AVG all",
	"q01_avg_by_linestatus" = "AVG all\nGROUP BY\nstatus",
	"q01_avg_by_returnflag" = "AVG all\nGROUP BY\nflag",
	"q01_avg_by_returnflag_linestatus" = "AVG all\nGROUP BY\nstatus,\nflag",
	"q01_sum_count_by_returnflag_linestatus" = "SUM\nCOUNT\nGROUP BY\nstatus,\nflag"
)

query_family <- c(
	"q01_avg_qty_all" = "AVG variants",
	"q01_avg_price_all" = "AVG variants",
	"q01_avg_disc_all" = "AVG variants",
	"q01_avg_all" = "AVG variants",
	"q01_avg_by_linestatus" = "AVG variants",
	"q01_avg_by_returnflag" = "AVG variants",
	"q01_avg_by_returnflag_linestatus" = "AVG variants",
	"q01_sum_count_by_returnflag_linestatus" = "SUM/COUNT variants"
)

m_levels <- c("m=64", "m=512")
m_colors <- c(
	"m=64" = "#4dff4d",
	"m=512" = "#009900"
)

raw <- bind_rows(lapply(input_csvs, function(input_csv) {
	suppressWarnings(readr::read_csv(input_csv, show_col_types = FALSE))
}))
plot_data <- raw %>%
	mutate(
		success = success_flag(success),
		query = as.character(query),
		sass_m = suppressWarnings(as.integer(sass_m)),
		sass_rescale = success_flag(sass_rescale),
		median_cv = suppressWarnings(as.numeric(median_cv)),
		cv_pct = 100.0 * median_cv,
		m_label = factor(paste0("m=", sass_m), levels = m_levels),
		family = factor(query_family[query], levels = c("AVG variants", "SUM/COUNT variants")),
		query_label = factor(query_labels[query], levels = query_labels)
	) %>%
	filter(
		success,
		query %in% names(query_labels),
		(
			(query != "q01_sum_count_by_returnflag_linestatus" & !sass_rescale) |
			(query == "q01_sum_count_by_returnflag_linestatus" & sass_rescale)
		),
		m_label %in% m_levels,
		!is.na(cv_pct),
		is.finite(cv_pct)
	) %>%
	mutate(
		query_label = factor(query_label, levels = query_labels),
		x_center = as.numeric(query_label)
	)

if (nrow(plot_data) == 0) {
	stop("No successful stability rows left after filtering.")
}

summary_csv <- sub("[.]png$", "_summary.csv", output_png)
readr::write_csv(plot_data, summary_csv)

base_theme <- theme_bw(base_size = 25, base_family = base_font) +
	theme(
		panel.border = element_rect(linewidth = 0.75),
		panel.grid.major = element_line(linewidth = 0.42),
		panel.grid.minor = element_blank(),
		legend.position = c(0.12, 0.88),
		legend.justification = c(0, 1),
		legend.title = element_blank(),
		legend.text = element_text(size = 21),
		legend.key.size = unit(0.64, "cm"),
		legend.background = element_rect(fill = alpha("white", 0.86), color = "grey50", linewidth = 0.25),
		legend.margin = margin(1, 4, 1, 4),
		legend.box.margin = margin(0, 0, 0, 0),
		axis.text.x = element_text(size = 16, angle = 35, hjust = 1, vjust = 1, lineheight = 0.82),
		axis.text.y = element_text(size = 17),
		axis.title = element_text(size = 18),
		axis.title.y = element_text(margin = margin(r = 5)),
		strip.background = element_blank(),
		strip.text = element_text(size = 18, face = "bold"),
		panel.spacing.x = unit(0.18, "cm"),
		plot.margin = margin(3, 8, 5, 14)
	)

p <- ggplot(plot_data, aes(x = query_label, y = cv_pct, fill = m_label)) +
	geom_vline(xintercept = 7.5, linetype = "dashed", linewidth = 0.45, color = "grey35") +
	geom_col(
		position = position_dodge(width = 0.72),
		width = 0.62,
		color = "grey25",
		linewidth = 0.2
	) +
	geom_text(
		data = plot_data %>% filter(family == "AVG variants"),
		aes(label = label_number(accuracy = 0.01)(cv_pct), group = m_label),
		position = position_dodge(width = 0.72),
		vjust = -0.25,
		size = 5.1,
		family = base_font
	) +
	scale_fill_manual(values = m_colors, drop = FALSE) +
	scale_y_continuous(labels = label_number(accuracy = 0.1), expand = expansion(mult = c(0, 0.08))) +
	labs(x = NULL, y = "lane CV (%)") +
	guides(fill = guide_legend(nrow = 1, byrow = TRUE)) +
	coord_cartesian(clip = "off") +
	base_theme

ggsave(output_png, p, width = 9.4, height = 3.2, dpi = 300, bg = "white")

message("Plot saved to: ", output_png)
message("Summary saved to: ", summary_csv)
