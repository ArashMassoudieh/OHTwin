# =============================================================================
# plot_paper_truth_vs_assim.gp
#
# Two-window comparison of truth and assimilation model outputs for the
# three calibrated observation channels of the Bioretention twin
# experiment. Each row is one channel; the row is split into a left
# panel (early-Phase 1 window, when the assimilation was still
# converging) and a right panel (later in Phase 1, after the
# assimilation had reached steady state). Y-axes are shared across the
# two panels of each row so magnitudes are directly comparable.
#
# Truth is rendered as small black markers (every Nth raw point);
# assimilation is a continuous line.
#
# Column layout in the OHQ selected-output CSVs (paired t,v):
#  11,12: Pond water depth (m)         <- calibrated
#  15,16: Soil Moisture                <- calibrated
#  17,18: Underdrain flow (m3/day)     <- calibrated
#
# Inputs:
#   Bioretention_truth/outputs/selected_output.csv
#   Bioretention_assimilation/outputs/selected_output.csv
#
# Output:
#   paper_truth_vs_assim.png
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file = "Bioretention_truth/outputs/selected_output.csv"
assim_file = "Bioretention_assimilation/outputs/selected_output.csv"
outfile    = "paper_truth_vs_assim.png"

# --- output canvas ----------------------------------------------------------
set terminal pngcairo size 1400,1300 enhanced font "Helvetica,18"
set output outfile

# --- time conversions ------------------------------------------------------
epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"

# --- window bounds ---------------------------------------------------------
early_start = 43865.0   # 2020-02-04
early_end   = 43875.0   # 2020-02-14
late_start  = 44047.0   # 2020-08-04
late_end    = 44057.0   # 2020-08-14

# --- truth sub-sampling ----------------------------------------------------
# At hourly resolution over a 10-day window (~240 points/channel), every
# point fits in the panel as a small marker without crowding.
sub_every = 1

# --- style -----------------------------------------------------------------
c_truth = "#000000"
c_assim = "#1f77b4"
ps_truth = 0.5
lw_assim = 1.8

date_format = "%b %d"

# --- layout: 3 rows x 2 columns --------------------------------------------
set multiplot layout 3,2

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

# ----- Row 1, left: Pond water depth (early window) -- legend lives here ---
set xrange [epoch(early_start):epoch(early_end)]
set format x ""
unset xlabel
set ylabel "Pond water depth (m)" offset 1,0
set yrange [0:0.17]
set ytics 0.05
set format y "%g"
set title "Early window: 2020-02-04 -- 2020-02-14" font "Helvetica,18"

set key top left inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0

plot \
    assim_file using (epoch($11)):12 with lines \
        lc rgb c_assim lw lw_assim title "Assimilation model", \
    truth_file every sub_every using (epoch($11)):12 with points \
        lc rgb c_truth pt 7 ps ps_truth title "Truth (synthetic obs.)"

unset key

# ----- Row 1, right: Pond water depth (late window) ------------------------
set xrange [epoch(late_start):epoch(late_end)]
set format x ""
unset ylabel
set yrange [0:0.17]
set format y ""
set title "Late window: 2020-08-04 -- 2020-08-14" font "Helvetica,18"

plot \
    assim_file using (epoch($11)):12 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($11)):12 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

# ----- Row 2, left: Soil moisture (early window) ---------------------------
set xrange [epoch(early_start):epoch(early_end)]
set format x ""
unset xlabel
set ylabel "Soil moisture" offset 1,0
set yrange [0.32:0.40]
set ytics 0.02
set format y "%g"
unset title

plot \
    assim_file using (epoch($15)):16 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($15)):16 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

# ----- Row 2, right: Soil moisture (late window) ---------------------------
set xrange [epoch(late_start):epoch(late_end)]
set format x ""
unset ylabel
set yrange [0.32:0.40]
set format y ""
unset title

plot \
    assim_file using (epoch($15)):16 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($15)):16 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

# ----- Row 3, left: Underdrain flow (early window) -- bottom row gets x-tics
set xrange [epoch(early_start):epoch(early_end)]
set xtics 172800   # 2 days in seconds
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
set ylabel "Underdrain flow (m^3/day)" offset 1,0
set yrange [0:700]
set ytics 200
set format y "%g"
unset title

plot \
    assim_file using (epoch($17)):18 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($17)):18 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

# ----- Row 3, right: Underdrain flow (late window) -------------------------
set xrange [epoch(late_start):epoch(late_end)]
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
unset ylabel
set yrange [0:700]
set format y ""
unset title

plot \
    assim_file using (epoch($17)):18 with lines \
        lc rgb c_assim lw lw_assim notitle, \
    truth_file every sub_every using (epoch($17)):18 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

unset multiplot
unset output
