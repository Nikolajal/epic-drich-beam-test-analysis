#!/usr/bin/env python3
r"""
generate_report.py -- Generate a LaTeX documentation report for the
epic-dRICH beam test tracking macros.

Usage:
    python3 generate_report.py [plots_dir] > tracking_report.tex

If plots_dir is given, \includegraphics will reference actual PNG files
found there. Missing files fall back to a framed placeholder.
"""

import os
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PLOTS_DIR = sys.argv[1] if len(sys.argv) > 1 else None


def esc(s):
    """Escape LaTeX special characters in plain text."""
    replacements = [
        ("\\", "\\textbackslash{}"),
        ("_", "\\_"),
        ("%", "\\%"),
        ("$", "\\$"),
        ("#", "\\#"),
        ("&", "\\&"),
        ("{", "\\{"),
        ("}", "\\}"),
        ("^", "\\textasciicircum{}"),
        ("~", "\\textasciitilde{}"),
    ]
    # We must handle backslash first to avoid double-escaping
    result = s
    for old, new in replacements:
        result = result.replace(old, new)
    return result


def fig(filename, caption, label, width=r"0.85\textwidth"):
    r"""Return a LaTeX figure environment for the given filename.

    If PLOTS_DIR is set and the file exists, use \includegraphics with an
    absolute path; otherwise emit a framed placeholder box.
    """
    if PLOTS_DIR is not None:
        abs_path = os.path.abspath(os.path.join(PLOTS_DIR, filename))
        if os.path.isfile(abs_path):
            graphic = (
                r"\includegraphics[width=" + width + r"]{" + abs_path + r"}"
            )
        else:
            graphic = _placeholder(filename)
    else:
        graphic = _placeholder(filename)

    return (
        r"\begin{figure}[H]" + "\n"
        r"  \centering" + "\n"
        "  " + graphic + "\n"
        r"  \caption{" + caption + r"}" + "\n"
        r"  \label{fig:" + label + r"}" + "\n"
        r"\end{figure}"
    )


def _placeholder(filename):
    """Return a LaTeX framed placeholder box for a missing figure."""
    return (
        r"\fbox{\parbox{0.8\textwidth}{\centering\small\texttt{"
        + esc(filename)
        + r"}\\[0.5em]\textit{[run macro to generate]}}}"
    )


def infobox(content):
    """Return a tcolorbox info block."""
    return (
        "\\begin{tcolorbox}[colback=blue!5!white, colframe=blue!50!black,\n"
        "  title=Overview, fonttitle=\\bfseries]\n"
        + content + "\n"
        "\\end{tcolorbox}"
    )


def formulabox(content):
    """Return a tcolorbox formula-summary block."""
    return (
        "\\begin{tcolorbox}[colback=green!5!white, colframe=green!40!black,\n"
        "  title=Formula Summary, fonttitle=\\bfseries]\n"
        + content + "\n"
        "\\end{tcolorbox}"
    )


# ---------------------------------------------------------------------------
# Preamble
# ---------------------------------------------------------------------------

PREAMBLE = r"""
\documentclass[a4paper,11pt]{article}

% --- packages ---------------------------------------------------------------
\usepackage[margin=2.5cm]{geometry}
\usepackage{amsmath}
\usepackage{amssymb}
\usepackage{graphicx}
\usepackage{booktabs}
\usepackage[colorlinks=true, linkcolor=blue, citecolor=blue, urlcolor=blue]{hyperref}
\usepackage{xcolor}
\usepackage{subcaption}
\usepackage{float}
\usepackage[most]{tcolorbox}
\usepackage{longtable}
\usepackage{array}
\usepackage{enumitem}

% --- metadata ---------------------------------------------------------------
\title{\textbf{epic-dRICH Beam Test: Tracking Macros}\\
  \large Technical Documentation\\[0.5em]
  \normalsize Auto-generated from \texttt{macros/tracking/} on \texttt{\today}}
\author{epic-dRICH Collaboration}
\date{\today}

\begin{document}
\maketitle
\tableofcontents
\listoffigures
\listoftables
\clearpage
"""

# ---------------------------------------------------------------------------
# Section 1 — Shared Concepts
# ---------------------------------------------------------------------------

SEC_INTRO = r"""
\section{Shared Concepts and Detector Geometry}
\label{sec:intro}

""" + infobox(
    r"This section describes conventions and mathematical objects that are shared by all"
    r" tracking macros in the \texttt{macros/tracking/} directory."
) + r"""

\subsection{Detector Coordinate System}

The ALTAI silicon-strip telescope defines the global reference frame.
The beam travels in the $-z$ direction.
Key $z$-positions of the detector planes are listed in Table~\ref{tab:zpositions}.

\begin{table}[H]
  \centering
  \caption{Nominal $z$-positions of the detector planes.}
  \label{tab:zpositions}
  \begin{tabular}{lrl}
    \toprule
    Detector & $z$ (mm) & Notes \\
    \midrule
    ALTAI telescope & $0$ & Upstream reference; track fit performed here \\
    Scintillator (plane1) & $-1150$ & Beam-trigger scintillator \\
    dRICH SiPM array (plane2) & $-4250$ & Same $z$ as aerogel radiator \\
    \bottomrule
  \end{tabular}
\end{table}

\subsection{Track Parametrisation}
\label{sec:track_param}

All macros represent a particle track as a straight line in 3D.
The parametrisation uses the tracker reference plane at $z=0$ as the intercept
origin:

\begin{align}
  x(z) &= p_x + s_x \cdot z, \label{eq:track_x}\\
  y(z) &= p_y + s_y \cdot z, \label{eq:track_y}
\end{align}

where:
\begin{itemize}[nosep]
  \item $p_x, p_y$ — track intercept at $z=0$ (tracker reference plane),
  \item $s_x = \tan(\alpha_x)$, $s_y = \tan(\alpha_y)$ — direction slopes in
        the $xz$ and $yz$ planes respectively.
\end{itemize}

The polar and azimuthal angles are:
\begin{align}
  \theta &= \arctan\!\left(\sqrt{s_x^2 + s_y^2}\right), \label{eq:theta}\\
  \varphi &= \mathrm{arctan2}(s_y,\, s_x). \label{eq:phi}
\end{align}

The track intercept at a detector plane located at $z = z_\mathrm{det}$ is:
\begin{align}
  x_\mathrm{det} &= p_x + s_x \cdot z_\mathrm{det}, \\
  y_\mathrm{det} &= p_y + s_y \cdot z_\mathrm{det}.
\end{align}

""" + formulabox(
    r"$x(z) = p_x + s_x z$,\quad $y(z) = p_y + s_y z$\\"
    r"$\theta = \arctan\!\sqrt{s_x^2+s_y^2}$,\quad"
    r"$\varphi = \mathrm{arctan2}(s_y, s_x)$"
) + r"""

\subsection{Hit Timing and Time-Window Cut}
\label{sec:timing_cut}

Each SiPM hit carries a timestamp $t_\mathrm{hit}$.  A beam trigger timestamp
$t_\mathrm{trig}$ is associated with each event.  The relative hit time is:
\begin{equation}
  \Delta t = t_\mathrm{hit} - t_\mathrm{trig}.
  \label{eq:dt}
\end{equation}

A hit is accepted into the analysis window if:
\begin{equation}
  t_\mathrm{min} \;\leq\; \Delta t \;\leq\; t_\mathrm{max}.
  \label{eq:window}
\end{equation}

Hits flagged as \emph{afterpulses} by the readout firmware are always
excluded regardless of their $\Delta t$.

\subsection{Geometric Track Selection with TCutG}
\label{sec:cutg}

Two configurable graphical cuts (\texttt{TCutG} objects from ROOT) are applied
to the track intercepts on two detector planes:

\begin{itemize}[nosep]
  \item \textbf{cutg1 / plane1} — evaluated at the scintillator ($z=-1150$\,mm).
        \emph{Convention: plane1 = scintillator, plane2 = dRICH. Never invert.}
  \item \textbf{cutg2 / plane2} — evaluated at the dRICH SiPM plane ($z=-4250$\,mm).
\end{itemize}

Named presets recognised by the configuration parser:
\begin{itemize}[nosep]
  \item \texttt{scint\_largo}, \texttt{scint\_stretto} — loose / tight cuts on scintillator,
  \item \texttt{drich\_largo}, \texttt{drich\_stretto} — loose / tight cuts on dRICH,
  \item \texttt{rect xmin xmax ymin ymax} — axis-aligned rectangular cut.
\end{itemize}

An optional anti-multiple-scattering cut rejects tracks with large polar angle:
\begin{equation}
  \theta \;<\; \theta_\mathrm{ms,max}.
  \label{eq:ms_cut}
\end{equation}
From beam data the core Gaussian width of the $\theta$ distribution is
$\sigma(\theta)_\mathrm{core} \approx 0.96\,\mathrm{mrad}$; a recommended value
is $\theta_\mathrm{ms,max} = 2\sigma \approx 0.002\,\mathrm{rad}$.
"""

# ---------------------------------------------------------------------------
# Section 2 — ringtrack_beam
# ---------------------------------------------------------------------------

SEC_BEAM = r"""
\section{\texttt{ringtrack\_beam} — Beam Composition and Vertex Finding}
\label{sec:beam}

""" + infobox(
    r"Characterises the upstream beam: reconstructed track multiplicity per event,"
    r" vertex finding via the distance of closest approach (DCA), and back-projection"
    r" displays for visual inspection of the beam geometry."
) + r"""

\subsection{Event Classification}

For each event the number of reconstructed ALTAI tracks $N_\mathrm{trk}$ is
counted.  Events are classified into five categories:
$N_\mathrm{trk} \in \{0, 1, 2, 3, \geq4\}$.
The fractional abundance of each category is reported as a percentage.

\subsection{Vertex Finding by Distance of Closest Approach}
\label{sec:dca}

For a pair of tracks $i$ and $j$ define the difference vectors:
\begin{align}
  \Delta p_x &= p_{x,i} - p_{x,j}, & \Delta p_y &= p_{y,i} - p_{y,j},\\
  \Delta s_x &= s_{x,i} - s_{x,j}, & \Delta s_y &= s_{y,i} - s_{y,j}.
\end{align}

The squared transverse separation as a function of $z$ is:
\begin{equation}
  D^2(z) = (\Delta p_x + \Delta s_x \cdot z)^2
           + (\Delta p_y + \Delta s_y \cdot z)^2.
  \label{eq:D2}
\end{equation}

Setting $\mathrm{d}D^2/\mathrm{d}z = 0$ yields the $z$-position of closest
approach:
\begin{equation}
  z_v = -\frac{\Delta p_x\,\Delta s_x + \Delta p_y\,\Delta s_y}
              {\Delta s_x^2 + \Delta s_y^2}.
  \label{eq:zv}
\end{equation}

The DCA is:
\begin{equation}
  \mathrm{DCA} = \sqrt{
    \bigl(x_i(z_v) - x_j(z_v)\bigr)^2 +
    \bigl(y_i(z_v) - y_j(z_v)\bigr)^2
  }.
  \label{eq:dca}
\end{equation}

The vertex position is taken as the midpoint:
\begin{align}
  x_v &= \frac{x_i(z_v) + x_j(z_v)}{2}, &
  y_v &= \frac{y_i(z_v) + y_j(z_v)}{2}.
  \label{eq:vertex_xy}
\end{align}

For events with more than two tracks the \emph{best vertex} is determined by
the pair with the smallest DCA among all $\binom{N_\mathrm{trk}}{2}$ pairs.
Only vertices satisfying $\mathrm{DCA} < \texttt{vertex\_dca\_max}$ are accepted.

""" + formulabox(
    r"$z_v = -\dfrac{\Delta p_x\,\Delta s_x + \Delta p_y\,\Delta s_y}"
    r"{\Delta s_x^2 + \Delta s_y^2}$\quad\quad"
    r"$\mathrm{DCA} = \sqrt{(\Delta x(z_v))^2+(\Delta y(z_v))^2}$"
) + r"""

\subsection{Back-Projection Display}

All reconstructed tracks are parametrised over a $z$-range
$[z_\mathrm{min}, z_\mathrm{max}]$ and projected into the $(z, x(z))$ and
$(z, y(z))$ planes, producing 2D density histograms.

In the \emph{from-vertex} variant:
\begin{itemize}[nosep]
  \item Single-track events: track drawn over the full $z$-range.
  \item Multi-track events: tracks drawn only from $z = z_v$ toward the
        downstream detectors.
\end{itemize}

\subsection{Configuration Parameters}

\begin{center}
\begin{tabular}{lll}
  \toprule
  Parameter & Default & Description \\
  \midrule
  \texttt{vertex\_dca\_max} & 5\,mm & Maximum DCA to accept a vertex \\
  \texttt{z\_min} & $-5000$\,mm & Upstream limit for back-projection \\
  \texttt{z\_max} & $200$\,mm & Downstream limit for back-projection \\
  \bottomrule
\end{tabular}
\end{center}

\subsection{Output Files}

\begin{center}
\begin{tabular}{ll}
  \toprule
  File & Content \\
  \midrule
  \texttt{beam.root} & All histograms and trees \\
  \texttt{beam\_stats.txt} & Summary statistics (multiplicity, vertex counts) \\
  \texttt{beam\_multiplicity.png} & Track multiplicity per event \\
  \texttt{beam\_chi2.png} & $\chi^2/\mathrm{NDF}$ distribution \\
  \texttt{beam\_theta.png} & Polar angle $\theta$ distribution \\
  \texttt{beam\_vertex\_z.png} & Vertex $z$-position \\
  \texttt{beam\_vertex\_dca.png} & DCA distribution \\
  \texttt{beam\_vertex\_xy.png} & Vertex $(x_v, y_v)$ map \\
  \texttt{beam\_backproj.png} & Back-projection density (all tracks) \\
  \texttt{beam\_backproj\_from\_vertex.png} & Back-projection from vertex \\
  \texttt{beam\_lines\_xz.png} & Individual track lines in $(z,x)$ \\
  \texttt{beam\_lines\_yz.png} & Individual track lines in $(z,y)$ \\
  \texttt{beam\_3d.png} & 3-D track display \\
  \bottomrule
\end{tabular}
\end{center}

""" + "\n".join([
    fig("beam_multiplicity.png",
        r"Track multiplicity per event. The fractional abundance of each category"
        r" ($N_\mathrm{trk}=0,1,2,3,\geq4$) is annotated.",
        "beam_mult"),
    fig("beam_theta.png",
        r"Polar angle $\theta$ distribution of reconstructed tracks."
        r" The core Gaussian width $\sigma_\mathrm{core}\approx0.96\,\mathrm{mrad}$"
        r" is extracted from a fit.",
        "beam_theta"),
    fig("beam_vertex_z.png",
        r"Distribution of reconstructed vertex $z$-positions.",
        "beam_vertex_z"),
    fig("beam_backproj.png",
        r"Back-projection density of all reconstructed tracks in the $(z,x)$ plane.",
        "beam_backproj"),
])

# ---------------------------------------------------------------------------
# Section 3 — ringtrack_notrack
# ---------------------------------------------------------------------------

SEC_NOTRACK = r"""
\section{\texttt{ringtrack\_notrack} — No-Track Event Study}
\label{sec:notrack}

""" + infobox(
    r"Classifies events by tracker and dRICH hit activity to diagnose beam-trigger"
    r" inefficiencies, out-of-time hits, and ALTAI--dRICH synchronisation."
) + r"""

\subsection{Event Categories}

Every event is assigned to exactly one of seven mutually exclusive categories
based on the number of reconstructed ALTAI tracks ($N_\mathrm{trk}$), the
number of dRICH hits within the time window ($N_\mathrm{hits,win}$), and the
total raw hit count ($N_\mathrm{raw}$):

\begin{center}
\begin{tabular}{llp{7cm}}
  \toprule
  Category & Condition & Interpretation \\
  \midrule
  \texttt{no\_trk\_0raw}  & $N_\mathrm{trk}=0$, $N_\mathrm{raw}=0$
    & Trigger completely empty — no activity anywhere \\
  \texttt{no\_trk\_0win}  & $N_\mathrm{trk}=0$, $N_\mathrm{hits,win}=0$, $N_\mathrm{raw}>0$
    & Hits exist but all outside the time window \\
  \texttt{no\_trk\_hits}  & $N_\mathrm{trk}=0$, $N_\mathrm{hits,win}>0$
    & dRICH fired, tracker did not reconstruct a track \\
  \texttt{1trk\_0win}     & $N_\mathrm{trk}=1$, $N_\mathrm{hits,win}=0$
    & Single track, no dRICH signal in window \\
  \texttt{1trk\_hits}     & $N_\mathrm{trk}=1$, $N_\mathrm{hits,win}>0$
    & Single track with dRICH signal (nominal) \\
  \texttt{multi\_0win}    & $N_\mathrm{trk}\geq2$, $N_\mathrm{hits,win}=0$
    & Multiple tracks, no dRICH signal \\
  \texttt{multi\_hits}    & $N_\mathrm{trk}\geq2$, $N_\mathrm{hits,win}>0$
    & Multiple tracks with dRICH signal \\
  \bottomrule
\end{tabular}
\end{center}

\subsection{Trigger Alignment Check}

The TTree entry index $i_\mathrm{frame}$ equals the ALTAI hardware trigger
counter.  Plotting $i_\mathrm{frame}$ vs.\ the \texttt{coarse} timestamp of
the beam trigger (a free-running counter with 25\,ns resolution) produces a
straight diagonal if every hardware trigger is present exactly once.
Deviations from linearity indicate dropped or duplicated triggers.

The trigger gap is monitored by the first-difference:
\begin{equation}
  \Delta\mathrm{coarse}(i) =
    \bigl[\mathrm{coarse}(i) - \mathrm{coarse}(i-1)\bigr] \bmod 65536.
  \label{eq:dcoarse}
\end{equation}

\subsection{Conditional Probability}

The conditional probability that the dRICH records zero hits in the time window
given $k$ reconstructed tracks is:
\begin{equation}
  P\!\left(N_\mathrm{hits}=0 \;\middle|\; N_\mathrm{trk}=k\right)
  = \frac{N(N_\mathrm{trk}=k,\; N_\mathrm{hits,win}=0)}{N(N_\mathrm{trk}=k)},
  \qquad k = 0, 1, 2, 3, \geq4.
  \label{eq:cond_prob}
\end{equation}

This is derived from the 2D alignment matrix histogram (tracker multiplicity
vs.\ dRICH hit count).

\subsection{Output Files}

\begin{center}
\begin{tabular}{ll}
  \toprule
  File & Content \\
  \midrule
  \texttt{notrack.root} & All histograms \\
  \texttt{notrack\_stats.txt} & Category counts and probabilities \\
  \texttt{notrack\_notrack\_frames.txt} & List of all $N_\mathrm{trk}=0$ event indices \\
  \texttt{notrack\_doubly\_empty\_frames.txt} & $N_\mathrm{trk}=0$ AND $N_\mathrm{hits,win}=0$ \\
  \texttt{notrack\_event\_table.tsv} & Full per-event table \\
  \texttt{notrack\_categories.png} & Bar chart of category counts \\
  \texttt{notrack\_timeline.png} & Category label vs.\ $i_\mathrm{frame}$ \\
  \texttt{notrack\_hitmap.png} & dRICH hit map for no-track events \\
  \texttt{notrack\_nhits.png} & $N_\mathrm{hits,win}$ distribution per category \\
  \texttt{notrack\_timing.png} & Hit $\Delta t$ for no-track events \\
  \texttt{notrack\_iframe\_coarse.png} & $i_\mathrm{frame}$ vs.\ \texttt{coarse} (alignment) \\
  \texttt{notrack\_coarse\_diff.png} & $\Delta\mathrm{coarse}$ distribution \\
  \texttt{notrack\_alignment\_matrix.png} & 2D: $N_\mathrm{trk}$ vs.\ $N_\mathrm{hits,win}$ \\
  \texttt{notrack\_alignment\_prob.png} & $P(N_\mathrm{hits}=0|N_\mathrm{trk}=k)$ \\
  \bottomrule
\end{tabular}
\end{center}

""" + "\n".join([
    fig("notrack_categories.png",
        r"Abundance of the seven event categories. The \texttt{no\_trk\_hits}"
        r" category is of particular interest for efficiency studies.",
        "notrack_categories"),
    fig("notrack_iframe_coarse.png",
        r"Trigger alignment: $i_\mathrm{frame}$ vs.\ \texttt{coarse} timestamp."
        r" A perfectly linear diagonal indicates no dropped or duplicated triggers.",
        "notrack_iframe"),
    fig("notrack_alignment_matrix.png",
        r"2D alignment matrix: ALTAI track multiplicity vs.\ dRICH hit count in the"
        r" time window. Off-diagonal population reveals coincidence inefficiency.",
        "notrack_matrix"),
])

# ---------------------------------------------------------------------------
# Section 4 — ringtrack_timing
# ---------------------------------------------------------------------------

SEC_TIMING = r"""
\section{\texttt{ringtrack\_timing} — Dominant Time Window}
\label{sec:timing}

""" + infobox(
    r"Identifies the dominant SiPM activity window within each event using a"
    r" sliding-bin scan, and correlates the window centre with track parameters."
) + r"""

\subsection{Method}

For each event the accepted hit times $\{\Delta t_k\}$ (equation~\ref{eq:dt})
are partitioned into bins of width $\Delta\tau$ covering the full range
$[t_\mathrm{min}, t_\mathrm{max}]$.  The bin index for a hit with time
$\Delta t$ is:
\begin{equation}
  b = \left\lfloor \frac{\Delta t - t_\mathrm{min}}{\Delta\tau} \right\rfloor.
  \label{eq:bin_index}
\end{equation}

The \emph{dominant bin} is defined as:
\begin{equation}
  b^* = \arg\max_b\; N_b,
  \label{eq:dominant_bin}
\end{equation}
where $N_b$ is the number of hits in bin $b$.

The centre of the dominant window is:
\begin{equation}
  t^* = t_\mathrm{min} + \left(b^* + \tfrac{1}{2}\right)\Delta\tau.
  \label{eq:tstar}
\end{equation}

The dominance ratio measures the fraction of hits concentrated in the dominant
bin:
\begin{equation}
  R = \frac{N_{b^*}}{N_\mathrm{total}}.
  \label{eq:dominance}
\end{equation}

For each reconstructed track, $t^*$ and $R$ are stored together with the track
identity card (track index, pixel position, $\theta$, $\chi^2/\mathrm{NDF}$,
intercepts) in a plain-text file.  Two-dimensional histograms of track
parameters vs.\ $t^*$ are filled to reveal any time-dependence of the beam
composition.

""" + formulabox(
    r"$b = \lfloor(\Delta t - t_\mathrm{min})/\Delta\tau\rfloor$,\quad"
    r"$b^* = \arg\max_b N_b$,\quad"
    r"$t^* = t_\mathrm{min} + (b^*+\tfrac{1}{2})\Delta\tau$,\quad"
    r"$R = N_{b^*}/N_\mathrm{total}$"
) + r"""

\subsection{Configuration Parameters}

\begin{center}
\begin{tabular}{lll}
  \toprule
  Parameter & Default & Description \\
  \midrule
  \texttt{timing\_bin\_width} & 5\,ns & Width $\Delta\tau$ of the scan bins \\
  \texttt{t\_min} & $-50$\,ns & Start of timing range \\
  \texttt{t\_max} & $200$\,ns & End of timing range \\
  \bottomrule
\end{tabular}
\end{center}

\subsection{Output Files}

\begin{center}
\begin{tabular}{ll}
  \toprule
  File & Content \\
  \midrule
  \texttt{track\_timing.root} & All histograms \\
  \texttt{track\_timing.txt} & Per-track identity cards with $t^*$ and $R$ \\
  \texttt{track\_timing\_t.png} & $\Delta t$ hit distribution \\
  \texttt{track\_timing\_dom.png} & Dominant window centre $t^*$ distribution \\
  \texttt{track\_timing\_ix\_iy.png} & 2D intercept map coloured by $t^*$ \\
  \texttt{track\_timing\_theta.png} & $\theta$ vs.\ $t^*$ \\
  \texttt{track\_timing\_chi2.png} & $\chi^2/\mathrm{NDF}$ vs.\ $t^*$ \\
  \texttt{track\_timing\_dominance.png} & Dominance ratio $R$ distribution \\
  \texttt{track\_timing\_compare\_ix.png} & $x$-intercept: dominant vs.\ others \\
  \texttt{track\_timing\_compare\_iy.png} & $y$-intercept: dominant vs.\ others \\
  \bottomrule
\end{tabular}
\end{center}

""" + "\n".join([
    fig("track_timing_dom.png",
        r"Distribution of the dominant window centre $t^*$ across all reconstructed"
        r" tracks. The peak identifies the nominal signal time.",
        "timing_dom"),
    fig("track_timing_dominance.png",
        r"Dominance ratio $R = N_{b^*}/N_\mathrm{total}$. Values close to 1 indicate"
        r" that the hit activity is concentrated in a single time bin.",
        "timing_dominance"),
])

# ---------------------------------------------------------------------------
# Section 5 — ringtrack_analysis
# ---------------------------------------------------------------------------

SEC_ANALYSIS = r"""
\section{\texttt{ringtrack\_analysis} — Main Hit Analysis}
\label{sec:analysis}

""" + infobox(
    r"For each event passing the geometric track selection, counts dRICH hits in"
    r" the time window and studies correlations between the hit count and the"
    r" reconstructed track parameters."
) + r"""

\subsection{Key Quantities}

The signal hit count is:
\begin{equation}
  n_\mathrm{hits} = \bigl|\{k : t_\mathrm{min} \leq \Delta t_k \leq t_\mathrm{max},\;
    \text{not afterpulse}\}\bigr|.
  \label{eq:nhits}
\end{equation}

The track intercept at the dRICH plane ($z = z_\mathrm{dRICH} = -4250$\,mm) is:
\begin{align}
  x_\mathrm{dRICH} &= p_x + s_x \cdot z_\mathrm{dRICH}, \\
  y_\mathrm{dRICH} &= p_y + s_y \cdot z_\mathrm{dRICH}.
  \label{eq:drich_intercept}
\end{align}

The goodness of the ALTAI track fit is quantified by $\chi^2/\mathrm{NDF}$,
provided directly by the ALTAI reconstruction software.

\subsection{Window Selection and Veto Logic}

Events may be selected or rejected based on hit counts in one or more
configurable time windows:

\begin{enumerate}[nosep]
  \item \textbf{Signal window selection:} require
    $n_\mathrm{hits}\in[\texttt{sel\_min},\texttt{sel\_max}] \geq \texttt{threshold}$.
  \item \textbf{Veto window 1:} reject if
    $n_\mathrm{hits}\in[\texttt{veto1\_min},\texttt{veto1\_max}] \geq \texttt{threshold}$.
  \item \textbf{Veto window 2:} same logic, independent second veto window.
\end{enumerate}

The veto windows suppress background from delayed de-excitation activity and
pre-trigger pileup.

\subsection{Configuration Parameters}

\begin{center}
\begin{tabular}{lll}
  \toprule
  Parameter & Default & Description \\
  \midrule
  \texttt{t\_min} / \texttt{t\_max} & run-dep. & Signal time window \\
  \texttt{apply\_window\_selection} & false & Enable signal window selection \\
  \texttt{apply\_window\_veto} & false & Enable veto window logic \\
  \texttt{sel\_min} / \texttt{sel\_max} & — & Selection window bounds (ns) \\
  \texttt{veto\_min} / \texttt{veto\_max} & — & First veto window bounds (ns) \\
  \texttt{veto2\_min} / \texttt{veto2\_max} & — & Second veto window bounds (ns) \\
  \bottomrule
\end{tabular}
\end{center}

\subsection{Output Files}

\begin{center}
\begin{tabular}{ll}
  \toprule
  File & Content \\
  \midrule
  \texttt{analysis.root} & All histograms \\
  \texttt{intercepts.txt} & Per-event track intercepts and hit counts \\
  \texttt{analysis\_t.png} & Hit $\Delta t$ distribution \\
  \texttt{analysis\_nhits.png} & $n_\mathrm{hits}$ distribution \\
  \texttt{analysis\_intercept.png} & Track intercept map at dRICH plane \\
  \texttt{analysis\_display.png} & Event display overlay \\
  \bottomrule
\end{tabular}
\end{center}

""" + "\n".join([
    fig("analysis_nhits.png",
        r"Distribution of the dRICH hit count $n_\mathrm{hits}$ for selected events."
        r" The signal peak and the pedestal (0-hit events) are visible.",
        "analysis_nhits"),
    fig("analysis_intercept.png",
        r"Track intercept positions at the dRICH SiPM plane"
        r" ($z_\mathrm{dRICH}=-4250$\,mm).",
        "analysis_intercept"),
])

# ---------------------------------------------------------------------------
# Section 6 — ringtrack_mult_windows
# ---------------------------------------------------------------------------

SEC_MULTWIN = r"""
\section{\texttt{ringtrack\_mult\_windows} — Multi-Window Comparison}
\label{sec:mult_windows}

""" + infobox(
    r"Compares the dRICH signal yield simultaneously across multiple configurable"
    r" time windows, enabling systematic studies of the optimal integration gate."
) + r"""

\subsection{Method}

For each selected event the hit count is evaluated for each of up to $N$
independently configurable time windows $[w_i^\mathrm{min},\, w_i^\mathrm{max}]$:
\begin{equation}
  n_\mathrm{hits}^{(i)} = \bigl|\{k : w_i^\mathrm{min} \leq \Delta t_k
    \leq w_i^\mathrm{max},\; \text{not afterpulse}\}\bigr|.
\end{equation}

A separate $n_\mathrm{hits}$ histogram is filled for each window $i$, allowing
direct visual comparison of the expected signal-to-noise ratio for different
gate widths (e.g.\ 12\,ns narrow gate vs.\ 70\,ns wide gate).

The track selection and TCutG cuts are applied identically for all windows;
the only difference between windows is the time range.

\subsection{Configuration Parameters}

\begin{center}
\begin{tabular}{lll}
  \toprule
  Parameter & Default & Description \\
  \midrule
  \texttt{nwindows} & 2 & Number of windows to compare \\
  \texttt{win\_i\_min} / \texttt{win\_i\_max} & run-dep. & Bounds of window $i$ (ns) \\
  \bottomrule
\end{tabular}
\end{center}
"""

# ---------------------------------------------------------------------------
# Section 7 — ringtrack_noring
# ---------------------------------------------------------------------------

SEC_NORING = r"""
\section{\texttt{ringtrack\_noring} — Ring / No-Ring Classification}
\label{sec:noring}

""" + infobox(
    r"Tags each event as ``ring'' or ``no-ring'' using one of three configurable"
    r" algorithms: a simple hit-count threshold, DBSCAN spatial clustering, or a"
    r" Hough transform circle finder."
) + r"""

\subsection{Method 1 — Hit-Count Threshold}

The simplest classifier: an event is labelled \emph{ring} if:
\begin{equation}
  n_\mathrm{hits} \;\geq\; \texttt{noring\_nhits\_threshold}.
  \label{eq:noring_threshold}
\end{equation}

\subsection{Method 2 — DBSCAN Clustering}
\label{sec:dbscan}

DBSCAN operates on the 3D feature space $(x_\mathrm{hit},\, y_\mathrm{hit},\,
\Delta t_\mathrm{hit})$.  Two hits $i$ and $j$ are \emph{neighbours} if their
normalised distance satisfies:
\begin{equation}
  d_{ij} = \sqrt{
    \left(\frac{\Delta x}{d_r}\right)^2 +
    \left(\frac{\Delta y}{d_r}\right)^2 +
    \left(\frac{\Delta t}{d_t}\right)^2
  } \;\leq\; 1,
  \label{eq:dbscan_dist}
\end{equation}
where $d_r$ and $d_t$ are the spatial and temporal neighbourhood radii.

A hit is a \emph{core point} if it has at least $\texttt{minPts}$ neighbours.
Clusters are formed by connecting core points and their border points.
An event is labelled \emph{ring} if at least one cluster is found.

\subsection{Method 3 — Hough Transform}
\label{sec:hough}

A look-up table (LUT) is precomputed: for each SiPM pixel $p$ (global index)
and each candidate radius $r \in [r_\mathrm{min}, r_\mathrm{max}]$ in steps
of $r_\mathrm{step}$, the set of accumulator cells $(a, b, r)$ that would be
voted by a hit at pixel $p$ with radius $r$ is stored.

For each event the accumulator is filled:
\begin{equation}
  A(a, b, r) \mathrel{+}= 1 \quad \forall\, \text{pixel } p,\; \text{radius } r
  \text{ in LUT},
\end{equation}
and the peak is located:
\begin{equation}
  (a^*, b^*, r^*) = \arg\max_{a,b,r} A(a, b, r).
\end{equation}

A hit at position $(x_i, y_i)$ is flagged as \emph{ring-tagged} if:
\begin{equation}
  \left|\sqrt{(x_i - a^*)^2 + (y_i - b^*)^2} \;-\; r^*\right|
  \;<\; \epsilon \cdot r^*,
  \label{eq:hough_tag}
\end{equation}
where $\epsilon = \texttt{hough\_tag\_fraction}$.  An event is labelled
\emph{ring} if the number of ring-tagged hits exceeds $\texttt{hough\_min\_hits}$.

""" + formulabox(
    r"\textbf{DBSCAN:} $d_{ij}=\sqrt{(\Delta x/d_r)^2+(\Delta y/d_r)^2+(\Delta t/d_t)^2}\leq1$\\"
    r"\textbf{Hough peak:} $(a^*,b^*,r^*)=\arg\max A(a,b,r)$\\"
    r"\textbf{Tag condition:} $\left|\sqrt{(x_i-a^*)^2+(y_i-b^*)^2}-r^*\right|<\epsilon r^*$"
) + r"""

\subsection{Configuration Parameters}

\begin{center}
\begin{tabular}{lll}
  \toprule
  Parameter & Default & Description \\
  \midrule
  \texttt{noring\_method} & 1 & Algorithm: 1=threshold, 2=DBSCAN, 3=Hough \\
  \texttt{noring\_nhits\_threshold} & 5 & Min.\ hits for ``ring'' (Method 1) \\
  \texttt{dbscan\_dr} & 3\,mm & Spatial radius $d_r$ (Method 2) \\
  \texttt{dbscan\_dt} & 10\,ns & Temporal radius $d_t$ (Method 2) \\
  \texttt{dbscan\_minpts} & 3 & Min.\ neighbours for core point (Method 2) \\
  \texttt{hough\_r\_min} & 10\,mm & Minimum candidate radius (Method 3) \\
  \texttt{hough\_r\_max} & 80\,mm & Maximum candidate radius (Method 3) \\
  \texttt{hough\_r\_step} & 1\,mm & Radius step in LUT (Method 3) \\
  \texttt{hough\_tag\_fraction} & 0.15 & Tagging tolerance $\epsilon$ (Method 3) \\
  \texttt{hough\_min\_hits} & 5 & Min.\ tagged hits for ``ring'' (Method 3) \\
  \bottomrule
\end{tabular}
\end{center}

\subsection{Output Files}

\begin{center}
\begin{tabular}{ll}
  \toprule
  File & Content \\
  \midrule
  \texttt{noring.root} & All histograms \\
  \texttt{noring\_stats.txt} & Ring/no-ring counts and fractions \\
  \texttt{noring\_display\_ring.png} & Event display: example ring events \\
  \texttt{noring\_display\_noring.png} & Event display: example no-ring events \\
  \bottomrule
\end{tabular}
\end{center}

""" + "\n".join([
    fig("noring_display_ring.png",
        r"Example display of events classified as ``ring'' by the active algorithm."
        r" Hit positions are shown on the SiPM pixel map.",
        "noring_ring"),
    fig("noring_display_noring.png",
        r"Example display of events classified as ``no-ring''.",
        "noring_noring"),
])

# ---------------------------------------------------------------------------
# Appendix A — Configuration Parameter Reference
# ---------------------------------------------------------------------------

APPENDIX_A = r"""
\appendix
\section{Configuration Parameter Reference}
\label{app:conf}

The following table lists all key parameters from \texttt{ringtrack.conf}.
Boolean values are \texttt{true}/\texttt{false}; distances are in mm; times
in ns unless otherwise noted.

\begin{center}
\begin{longtable}{>{\ttfamily}p{4.8cm} p{1.6cm} p{2.2cm} p{5.5cm} p{2.2cm}}
  \toprule
  \normalfont Parameter & Type & Default & Description & Used by \\
  \midrule
  \endhead
  \bottomrule
  \endfoot
  % --- geometry ---
  z\_scint & float & $-1150$ & $z$-position of scintillator & all \\
  z\_drich & float & $-4250$ & $z$-position of dRICH SiPM plane & all \\
  % --- timing ---
  t\_min & float & run-dep. & Start of time window (ns) & all \\
  t\_max & float & run-dep. & End of time window (ns) & all \\
  timing\_bin\_width & float & 5 & Scan bin width $\Delta\tau$ (ns) & timing \\
  % --- track selection ---
  cutg1\_name & string & scint\_largo & Named preset for plane1 cut & all \\
  cutg2\_name & string & drich\_largo & Named preset for plane2 cut & all \\
  theta\_ms\_max & float & 0.002 & Anti-MS polar angle cut (rad) & all \\
  chi2\_max & float & 10 & Maximum $\chi^2/\mathrm{NDF}$ & all \\
  % --- beam macro ---
  vertex\_dca\_max & float & 5 & Max.\ DCA to accept vertex (mm) & beam \\
  z\_min & float & $-5000$ & Upstream back-projection limit & beam \\
  z\_max & float & 200 & Downstream back-projection limit & beam \\
  % --- analysis ---
  apply\_window\_selection & bool & false & Enable signal window & analysis \\
  apply\_window\_veto & bool & false & Enable veto window & analysis \\
  sel\_min / sel\_max & float & — & Selection window bounds (ns) & analysis \\
  veto\_min / veto\_max & float & — & Veto 1 window bounds (ns) & analysis \\
  veto2\_min / veto2\_max & float & — & Veto 2 window bounds (ns) & analysis \\
  % --- multi-window ---
  nwindows & int & 2 & Number of comparison windows & mult\_windows \\
  win\_i\_min / win\_i\_max & float & — & Bounds of window $i$ (ns) & mult\_windows \\
  % --- noring ---
  noring\_method & int & 1 & Ring finder: 1=thr, 2=DBSCAN, 3=Hough & noring \\
  noring\_nhits\_threshold & int & 5 & Min.\ hits (Method 1) & noring \\
  dbscan\_dr & float & 3 & Spatial radius $d_r$ (mm, Method 2) & noring \\
  dbscan\_dt & float & 10 & Temporal radius $d_t$ (ns, Method 2) & noring \\
  dbscan\_minpts & int & 3 & Min.\ neighbours (Method 2) & noring \\
  hough\_r\_min & float & 10 & Min.\ candidate radius (mm, Method 3) & noring \\
  hough\_r\_max & float & 80 & Max.\ candidate radius (mm, Method 3) & noring \\
  hough\_r\_step & float & 1 & LUT radius step (mm, Method 3) & noring \\
  hough\_tag\_fraction & float & 0.15 & Tag tolerance $\epsilon$ (Method 3) & noring \\
  hough\_min\_hits & int & 5 & Min.\ tagged hits (Method 3) & noring \\
\end{longtable}
\end{center}
"""

# ---------------------------------------------------------------------------
# Appendix B — Output File Reference
# ---------------------------------------------------------------------------

APPENDIX_B = r"""
\section{Output File Reference}
\label{app:outputs}

\begin{center}
\begin{longtable}{>{\ttfamily}p{6.5cm} p{3.5cm} p{5.5cm}}
  \toprule
  \normalfont File & Macro & Description \\
  \midrule
  \endhead
  \bottomrule
  \endfoot
  % beam
  beam.root & ringtrack\_beam & ROOT histograms and trees \\
  beam\_stats.txt & ringtrack\_beam & Summary statistics \\
  beam\_multiplicity.png & ringtrack\_beam & Track multiplicity histogram \\
  beam\_chi2.png & ringtrack\_beam & $\chi^2/\mathrm{NDF}$ distribution \\
  beam\_theta.png & ringtrack\_beam & Polar angle distribution \\
  beam\_vertex\_z.png & ringtrack\_beam & Vertex $z$-position \\
  beam\_vertex\_dca.png & ringtrack\_beam & DCA distribution \\
  beam\_vertex\_xy.png & ringtrack\_beam & Vertex $(x_v,y_v)$ map \\
  beam\_backproj.png & ringtrack\_beam & Track back-projection \\
  beam\_backproj\_from\_vertex.png & ringtrack\_beam & Back-projection from vertex \\
  beam\_lines\_xz.png & ringtrack\_beam & Track lines $(z,x)$ \\
  beam\_lines\_yz.png & ringtrack\_beam & Track lines $(z,y)$ \\
  beam\_3d.png & ringtrack\_beam & 3-D track display \\
  % notrack
  notrack.root & ringtrack\_notrack & ROOT histograms \\
  notrack\_stats.txt & ringtrack\_notrack & Category counts \\
  notrack\_notrack\_frames.txt & ringtrack\_notrack & $N_\mathrm{trk}=0$ frame list \\
  notrack\_doubly\_empty\_frames.txt & ringtrack\_notrack & Doubly empty frames \\
  notrack\_event\_table.tsv & ringtrack\_notrack & Full per-event table \\
  notrack\_categories.png & ringtrack\_notrack & Category bar chart \\
  notrack\_timeline.png & ringtrack\_notrack & Category timeline \\
  notrack\_hitmap.png & ringtrack\_notrack & dRICH hit map \\
  notrack\_nhits.png & ringtrack\_notrack & $N_\mathrm{hits,win}$ histogram \\
  notrack\_timing.png & ringtrack\_notrack & Hit timing \\
  notrack\_iframe\_coarse.png & ringtrack\_notrack & Alignment check \\
  notrack\_coarse\_diff.png & ringtrack\_notrack & Trigger gap $\Delta$coarse \\
  notrack\_alignment\_matrix.png & ringtrack\_notrack & 2D alignment matrix \\
  notrack\_alignment\_prob.png & ringtrack\_notrack & Conditional probability \\
  % timing
  track\_timing.root & ringtrack\_timing & ROOT histograms \\
  track\_timing.txt & ringtrack\_timing & Track identity cards \\
  track\_timing\_t.png & ringtrack\_timing & Hit $\Delta t$ distribution \\
  track\_timing\_dom.png & ringtrack\_timing & Dominant window $t^*$ \\
  track\_timing\_ix\_iy.png & ringtrack\_timing & Intercept map vs.\ $t^*$ \\
  track\_timing\_theta.png & ringtrack\_timing & $\theta$ vs.\ $t^*$ \\
  track\_timing\_chi2.png & ringtrack\_timing & $\chi^2$ vs.\ $t^*$ \\
  track\_timing\_dominance.png & ringtrack\_timing & Dominance ratio $R$ \\
  track\_timing\_compare\_ix.png & ringtrack\_timing & $x$-intercept comparison \\
  track\_timing\_compare\_iy.png & ringtrack\_timing & $y$-intercept comparison \\
  % analysis
  analysis.root & ringtrack\_analysis & ROOT histograms \\
  intercepts.txt & ringtrack\_analysis & Per-event intercepts \\
  analysis\_t.png & ringtrack\_analysis & Hit $\Delta t$ \\
  analysis\_nhits.png & ringtrack\_analysis & $n_\mathrm{hits}$ histogram \\
  analysis\_intercept.png & ringtrack\_analysis & Intercept map \\
  analysis\_display.png & ringtrack\_analysis & Event display \\
  % noring
  noring.root & ringtrack\_noring & ROOT histograms \\
  noring\_stats.txt & ringtrack\_noring & Ring/no-ring counts \\
  noring\_display\_ring.png & ringtrack\_noring & Ring event display \\
  noring\_display\_noring.png & ringtrack\_noring & No-ring event display \\
\end{longtable}
\end{center}

\end{document}
"""

# ---------------------------------------------------------------------------
# Main — assemble and print
# ---------------------------------------------------------------------------

def main():
    parts = [
        PREAMBLE,
        SEC_INTRO,
        SEC_BEAM,
        SEC_NOTRACK,
        SEC_TIMING,
        SEC_ANALYSIS,
        SEC_MULTWIN,
        SEC_NORING,
        APPENDIX_A,
        APPENDIX_B,
    ]
    print("\n".join(parts))


if __name__ == "__main__":
    main()
