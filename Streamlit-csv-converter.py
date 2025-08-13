import streamlit as st
import pandas as pd
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

def create_map(df):
    # Normalize speed values for colors
    norm = mcolors.Normalize(vmin=df['Speed'].min(), vmax=df['Speed'].max())
    cmap = cm.get_cmap('coolwarm')

    # Create Folium map
    m = folium.Map(location=[df.Lat.iloc[0], df.Lng.iloc[0]], zoom_start=12)

    # Draw colored path
    for i in range(1, len(df)):
        color = mcolors.to_hex(cmap(norm(df['Speed'].iloc[i])))
        folium.PolyLine(
            locations=[(df.Lat.iloc[i-1], df.Lng.iloc[i-1]),
                       (df.Lat.iloc[i], df.Lng.iloc[i])],
            color=color,
            weight=4,
            opacity=0.8
        ).add_to(m)

    # Add start pin
    folium.Marker(
        location=(df.Lat.iloc[0], df.Lng.iloc[0]),
        popup="Start",
        icon=folium.Icon(color="green", icon="play")
    ).add_to(m)

    # Add end pin
    folium.Marker(
        location=(df.Lat.iloc[-1], df.Lng.iloc[-1]),
        popup="End",
        icon=folium.Icon(color="blue", icon="stop")
    ).add_to(m)

    # Add fastest speed pin
    max_speed_idx = df['Speed'].idxmax()
    popup_text = f"Fastest Point: {df['Speed'].iloc[max_speed_idx]:.1f} mph"
    if pd.api.types.is_datetime64_any_dtype(df['Time']):
        popup_text += f"<br>Time: {df['Time'].iloc[max_speed_idx]}"
    folium.Marker(
        location=(df.Lat.iloc[max_speed_idx], df.Lng.iloc[max_speed_idx]),
        popup=folium.Popup(popup_text, max_width=250),
        icon=folium.Icon(color="red", icon="flag")
    ).add_to(m)

    # Add legend for speed
    colormap = LinearColormap(
        colors=[mcolors.to_hex(cmap(0)), mcolors.to_hex(cmap(0.5)), mcolors.to_hex(cmap(1))],
        vmin=df['Speed'].min(),
        vmax=df['Speed'].max(),
        caption='Speed (MPH)'
    )
    colormap.add_to(m)

    return m

if uploaded_file:
    try:
        df = pd.read_csv(uploaded_file, names=['Lat', 'Lng', 'Speed', 'Time', 'RPM'], on_bad_lines='skip')
    except Exception as e:
        st.error(f"Error reading CSV: {e}")
        st.stop()

    # Convert all numeric columns safely
    for col in ['Lat', 'Lng', 'Speed', 'RPM']:
        df[col] = pd.to_numeric(df[col], errors='coerce')

    # Drop rows with invalid data
    df = df.dropna()
    df = df[
        (df['Lat'].between(-90, 90)) &
        (df['Lng'].between(-180, 180)) &
        (df['Speed'] >= 0) &
        (df['RPM'] >= 0)
    ]

    # Try to parse time
    try:
        df['Time'] = pd.to_datetime(df['Time'], unit='s', utc=True)
    except Exception:
        pass

    if df.empty:
        st.error("No valid GPS data found after cleaning.")
        st.stop()

    # Calculate stats
    top_speed = df['Speed'].max()
    avg_speed = df['Speed'].mean()

    if pd.api.types.is_datetime64_any_dtype(df['Time']):
        total_time_sec = (df['Time'].iloc[-1] - df['Time'].iloc[0]).total_seconds()
        total_time_str = str(datetime.timedelta(seconds=int(total_time_sec)))
    else:
        total_time_sec = None
        total_time_str = "Unknown"

    total_distance_miles = sum(
        geodesic((df.Lat.iloc[i-1], df.Lng.iloc[i-1]),
                 (df.Lat.iloc[i], df.Lng.iloc[i])).miles
        for i in range(1, len(df))
    )

    # Summary stats
    st.markdown(f"### Summary Statistics")
    st.markdown(f"- **Top Speed:** {top_speed:.1f} mph")
    st.markdown(f"- **Average Speed:** {avg_speed:.1f} mph")
    st.markdown(f"- **Total Time:** {total_time_str}")
    st.markdown(f"- **Total Distance:** {total_distance_miles:.2f} miles")

    # Map
    st.subheader("Interactive GPS Map")
    m = create_map(df)
    st_folium(m, width=700, height=500)

    # Speed & RPM plot
    st.subheader("Speed & RPM Over Time")
    fig, ax1 = plt.subplots(figsize=(10, 5))

    ax1.set_xlabel("Time (UTC)")
    ax1.set_ylabel("Speed (MPH)", color="tab:red")
    ax1.plot(df['Time'], df['Speed'], color="tab:red", label="Speed")
    ax1.tick_params(axis='y', labelcolor="tab:red")

    ax2 = ax1.twinx()
    ax2.set_ylabel("RPM", color="tab:blue")
    ax2.plot(df['Time'], df['RPM'], color="tab:blue", label="RPM")
    ax2.tick_params(axis='y', labelcolor="tab:blue")

    fig.tight_layout()
    plt.title("Speed & RPM Over Time")
    st.pyplot(fig)

else:
    st.info("Upload a CSV file to get started.")


