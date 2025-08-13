import streamlit as st
import pandas as pd
import numpy as np
import folium
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import matplotlib.cm as cm
from streamlit_folium import st_folium
import datetime
from geopy.distance import geodesic
from branca.colormap import LinearColormap

st.title("Mini Logger V4.1 CSV Map Converter")

uploaded_file = st.file_uploader("Upload CSV file", type=["csv"])

REQUIRED_COLS = ["Lat", "Lng", "Speed", "Time", "RPM"]

def read_csv_flex(file):
    """
    Try to read with header; if required columns missing, retry as headerless with known names.
    """
    try:
        df = pd.read_csv(file, on_bad_lines="skip")
    except Exception:
        file.seek(0)
        df = pd.read_csv(file, names=REQUIRED_COLS, on_bad_lines="skip")

    has_all = all(c in df.columns for c in REQUIRED_COLS)
    if not has_all:
        # retry as headerless with names
        file.seek(0)
        df = pd.read_csv(file, names=REQUIRED_COLS, on_bad_lines="skip")

    # Final sanity: ensure all columns exist
    for c in REQUIRED_COLS:
        if c not in df.columns:
            df[c] = np.nan
    return df[REQUIRED_COLS].copy()

def drop_leading_blank_rows(df):
    """
    Drop leading rows that are clearly placeholders (0/0 coords, or NaNs).
    Keeps interior rows that may be zero-speed but valid coords.
    """
    valid_coord = (
        df["Lat"].between(-90, 90)
        & df["Lng"].between(-180, 180)
        & ~((df["Lat"].fillna(0) == 0) & (df["Lng"].fillna(0) == 0))
    )
    valid_idx = np.where(valid_coord.values)[0]
    if len(valid_idx) == 0:
        return df.iloc[0:0]  # empty
    first = valid_idx[0]
    return df.iloc[first:].reset_index(drop=True)

def parse_time_column(df):
    """
    Robust time parsing:
    - Try to_datetime (ISO-like strings)
    - If that fails a lot, try epoch seconds
    Returns df with a parsed UTC datetime 'Time' column (or leaves NaT where impossible).
    """
    # If already datetime-like, standardize to UTC
    if pd.api.types.is_datetime64_any_dtype(df["Time"]):
        df["Time"] = pd.to_datetime(df["Time"], utc=True, errors="coerce")
        return df

    # First try string -> datetime
    t1 = pd.to_datetime(df["Time"], utc=True, errors="coerce")
    valid1 = t1.notna().sum()

    # If not enough valid, try numeric epoch seconds
    if valid1 < len(df) * 0.5:
        tnum = pd.to_numeric(df["Time"], errors="coerce")
        t2 = pd.to_datetime(tnum, unit="s", utc=True, errors="coerce")
        valid2 = t2.notna().sum()
        df["Time"] = t2 if valid2 >= valid1 else t1
    else:
        df["Time"] = t1
    return df

def compute_total_time(df):
    """
    Total trip time = last valid timestamp - first valid timestamp (after cleaning).
    Returns (timedelta or None, nicely formatted string).
    """
    if not pd.api.types.is_datetime64_any_dtype(df["Time"]):
        return None, "Unknown"

    times = df["Time"].dropna()
    if times.empty:
        return None, "Unknown"

    total = times.iloc[-1] - times.iloc[0]
    seconds = int(total.total_seconds())
    if seconds < 0:
        return None, "Unknown"
    return total, str(datetime.timedelta(seconds=seconds))

def create_map(df):
    """
    Folium map with speed-colored path, start/end pins, fastest flag, and color legend.
    """
    # Normalize speed values for colors
    norm = mcolors.Normalize(vmin=df["Speed"].min(), vmax=df["Speed"].max())
    cmap = cm.get_cmap("coolwarm")

    center_lat = float(df["Lat"].iloc[0])
    center_lng = float(df["Lng"].iloc[0])
    m = folium.Map(location=[center_lat, center_lng], zoom_start=12)

    # Draw colored path
    if len(df) >= 2:
        for i in range(1, len(df)):
            color = mcolors.to_hex(cmap(norm(df["Speed"].iloc[i])))
            folium.PolyLine(
                locations=[(df.Lat.iloc[i - 1], df.Lng.iloc[i - 1]),
                           (df.Lat.iloc[i], df.Lng.iloc[i])],
                color=color,
                weight=4,
                opacity=0.8,
            ).add_to(m)

    # Add start pin
    folium.Marker(
        location=(df.Lat.iloc[0], df.Lng.iloc[0]),
        popup="Start",
        icon=folium.Icon(color="green", icon="play"),
    ).add_to(m)

    # Add end pin
    folium.Marker(
        location=(df.Lat.iloc[-1], df.Lng.iloc[-1]),
        popup="End",
        icon=folium.Icon(color="blue", icon="stop"),
    ).add_to(m)

    # Add fastest speed pin
    max_speed_idx = df["Speed"].idxmax()
    popup_text = f"Fastest Point: {df['Speed'].iloc[max_speed_idx]:.1f} mph"
    if pd.api.types.is_datetime64_any_dtype(df["Time"]):
        popup_text += f"<br>Time: {df['Time'].iloc[max_speed_idx]}"

    folium.Marker(
        location=(df.Lat.iloc[max_speed_idx], df.Lng.iloc[max_speed_idx]),
        popup=folium.Popup(popup_text, max_width=260),
        icon=folium.Icon(color="red", icon="flag"),
    ).add_to(m)

    # Add legend for speed
    colormap = LinearColormap(
        colors=[mcolors.to_hex(cmap(0)), mcolors.to_hex(cmap(0.5)), mcolors.to_hex(cmap(1))],
        vmin=float(df["Speed"].min()),
        vmax=float(df["Speed"].max()),
        caption="Speed (MPH)",
    )
    colormap.add_to(m)

    return m

def compute_distance_miles(df):
    """
    Sum geodesic distance (miles) between sequential valid points.
    """
    if len(df) < 2:
        return 0.0
    total = 0.0
    for i in range(1, len(df)):
        total += geodesic(
            (df.Lat.iloc[i - 1], df.Lng.iloc[i - 1]),
            (df.Lat.iloc[i], df.Lng.iloc[i]),
        ).miles
    return total

if uploaded_file:
    # ---------- LOAD ----------
    df = read_csv_flex(uploaded_file)

    # ---------- CLEAN NUMERICS ----------
    for col in ["Lat", "Lng", "Speed", "RPM"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    # Drop obvious garbage rows globally
    df = df.dropna(subset=["Lat", "Lng", "Speed"])  # keep RPM NaN if needed; we'll coerce to 0 later
    df = df[
        (df["Lat"].between(-90, 90))
        & (df["Lng"].between(-180, 180))
        & (df["Speed"] >= 0)
    ]

    # Drop only the LEADING placeholder rows (0/0 coords etc.), keep interior zero-speed if coords are valid
    df = drop_leading_blank_rows(df)

    # RPM: if missing, set to 0
    df["RPM"] = pd.to_numeric(df["RPM"], errors="coerce").fillna(0).clip(lower=0)

    # ---------- TIME PARSING ----------
    df = parse_time_column(df)

    # If still NaT everywhere, we cannot compute trip time; map and plots can still work
    if df.empty:
        st.error("No valid GPS data found after cleaning.")
        st.stop()

    # ---------- STATS ----------
    top_speed = float(df["Speed"].max())
    avg_speed = float(df["Speed"].mean())
    total_distance_miles = compute_distance_miles(df)

    total_td, total_time_str = compute_total_time(df)

    # ---------- SUMMARY ----------
    st.markdown("### Summary Statistics")
    st.markdown(f"- **Top Speed:** {top_speed:.1f} mph")
    st.markdown(f"- **Average Speed:** {avg_speed:.1f} mph")
    st.markdown(f"- **Total Distance:** {total_distance_miles:.2f} miles")
    st.markdown(f"- **Total Trip Time:** {total_time_str}")

    # ---------- MAP ----------
    st.subheader("Interactive GPS Map")
    try:
        m = create_map(df)
        st_folium(m, width=800, height=520)
    except Exception as e:
        st.warning(f"Could not render map: {e}")

    # ---------- PLOTS ----------
    st.subheader("Speed & RPM Over Time")
    fig, ax1 = plt.subplots(figsize=(10, 5))
    if pd.api.types.is_datetime64_any_dtype(df["Time"]):
        x = df["Time"]
        ax1.set_xlabel("Time (UTC)")
    else:
        x = df.index
        ax1.set_xlabel("Sample Index")

    ax1.set_ylabel("Speed (MPH)", color="tab:red")
    ax1.plot(x, df["Speed"], color="tab:red", label="Speed")
    ax1.tick_params(axis="y", labelcolor="tab:red")

    ax2 = ax1.twinx()
    ax2.set_ylabel("RPM", color="tab:blue")
    ax2.plot(x, df["RPM"], color="tab:blue", label="RPM")
    ax2.tick_params(axis="y", labelcolor="tab:blue")

    fig.tight_layout()
    plt.title("Speed & RPM Over Time")
    st.pyplot(fig)

    # ---------- OPTIONAL: DOWNLOAD CLEANED CSV ----------
    cleaned = df.copy()
    # Standardize Time column to ISO UTC string for export
    if pd.api.types.is_datetime64_any_dtype(cleaned["Time"]):
        cleaned["Time"] = cleaned["Time"].dt.strftime("%Y-%m-%d %H:%M:%S%z")
    csv_bytes = cleaned.to_csv(index=False).encode("utf-8")
    st.download_button(
        label="Download Cleaned CSV",
        data=csv_bytes,
        file_name="cleaned_log.csv",
        mime="text/csv",
    )
else:
    st.info("Upload a CSV file to get started.")
