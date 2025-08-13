import streamlit as st
import pandas as pd
import folium
import matplotlib.pyplot as plt
import matplotlib.cm as cm
from streamlit_folium import st_folium
from geopy.distance import geodesic
from branca.colormap import LinearColormap
import datetime

st.title("Mini Logger V4.1 CSV Map Converter")

uploaded_file = st.file_uploader("Upload CSV file", type=["csv"])

if uploaded_file:
    # Load CSV
    df = pd.read_csv(uploaded_file)

    # --- CLEANUP ---
    # Remove invalid GPS points (blank 00 rows, zero coords, zero speed)
    df = df[(df['Lat'] != 0) & (df['Lng'] != 0) & (df['Speed'] != 0)]

    # Remove rows with missing or bad timestamps
    if 'Time' in df.columns:
        try:
            df['Time'] = pd.to_datetime(df['Time'], errors='coerce', utc=True)
            df = df.dropna(subset=['Time'])
        except Exception as e:
            st.warning(f"Time parsing error: {e}")

    # Downsample for performance if needed
    if len(df) > 5000:
        df = df.iloc[::2].reset_index(drop=True)

    if df.empty:
        st.error("No valid GPS data found in this file.")
    else:
        # Calculate distance
        total_distance = 0
        for i in range(1, len(df)):
            total_distance += geodesic(
                (df.iloc[i-1]['Lat'], df.iloc[i-1]['Lng']),
                (df.iloc[i]['Lat'], df.iloc[i]['Lng'])
            ).meters

        # Calculate total trip time
        if 'Time' in df.columns:
            start_time = df['Time'].min()
            end_time = df['Time'].max()
            trip_duration = end_time - start_time
        else:
            trip_duration = "N/A"

        # --- MAP ---
        avg_lat, avg_lng = df['Lat'].mean(), df['Lng'].mean()
        m = folium.Map(location=[avg_lat, avg_lng], zoom_start=14)

        # Speed color mapping
        min_speed = df['Speed'].min()
        max_speed = df['Speed'].max()
        colormap = LinearColormap(cm.plasma.colors, vmin=min_speed, vmax=max_speed)

        coords = list(zip(df['Lat'], df['Lng']))
        for i in range(len(coords) - 1):
            speed = df.iloc[i]['Speed']
            folium.PolyLine(
                [coords[i], coords[i+1]],
                color=colormap(speed),
                weight=3
            ).add_to(m)

        colormap.caption = "Speed (units)"
        colormap.add_to(m)

        # Display map
        st_folium(m, width=800, height=500)

        # --- SUMMARY ---
        st.subheader("Trip Summary")
        st.write(f"**Total Distance:** {total_distance/1000:.2f} km")
        st.write(f"**Average Speed:** {df['Speed'].mean():.2f}")
        st.write(f"**Max Speed:** {df['Speed'].max():.2f}")
        st.write(f"**Total Trip Time:** {trip_duration}")

        # --- SPEED GRAPH ---
        plt.figure(figsize=(10, 4))
        plt.plot(df['Time'], df['Speed'], label="Speed")
        plt.xlabel("Time")
        plt.ylabel("Speed")
        plt.title("Speed over Time")
        plt.grid(True)
        plt.legend()
        st.pyplot(plt)

        # Download cleaned CSV
        csv = df.to_csv(index=False).encode('utf-8')
        st.download_button(
            label="Download Cleaned CSV",
            data=csv,
            file_name="cleaned_log.csv",
            mime="text/csv",
        )
