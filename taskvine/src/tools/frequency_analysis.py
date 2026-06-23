

import numpy as np
import matplotlib.pyplot as plt

if __name__ == "__main__":
    signals = [3, 2, 2, 1]
    
    # Parameters
    duration = 1.0  # seconds
    sample_rate = 1000  # Hz
    t = np.linspace(0, duration, int(sample_rate * duration))
    
    # Generate sine waves for each frequency and sum them
    combined_signal = np.zeros_like(t)
    for frequency in signals:
        sine_wave = np.sin(2 * np.pi * frequency * t)
        combined_signal += sine_wave

    # Compute FFT (Fourier series)
    fft_values = np.fft.fft(combined_signal)
    fft_frequencies = np.fft.fftfreq(len(t), 1/sample_rate)
    fft_magnitude = np.abs(fft_values) / len(t)
    
    # Get dominant components (only positive frequencies)
    positive_freq_idx = fft_frequencies > 0
    dominant_freq = fft_frequencies[positive_freq_idx]
    dominant_mag = fft_magnitude[positive_freq_idx]
    
    # Find top 5 dominant components
    top_indices = np.argsort(dominant_mag)[-5:][::-1]
    top_frequencies = dominant_freq[top_indices]
    top_magnitudes = dominant_mag[top_indices]
    
    # Plot the combined signal and frequency components
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # Time domain signal
    axes[0, 0].plot(t, combined_signal, linewidth=1, color='blue')
    axes[0, 0].set_xlabel('Time (s)')
    axes[0, 0].set_ylabel('Amplitude')
    axes[0, 0].set_title('Combined Sine Wave Signal')
    axes[0, 0].grid(True, alpha=0.3)
    
    # Full frequency spectrum
    axes[0, 1].plot(dominant_freq, dominant_mag, linewidth=1, color='green')
    axes[0, 1].set_xlabel('Frequency (Hz)')
    axes[0, 1].set_ylabel('Magnitude')
    axes[0, 1].set_title('Full Frequency Spectrum')
    axes[0, 1].grid(True, alpha=0.3)
    axes[0, 1].set_xlim(0, 20)
    
    # Dominant components bar chart
    axes[1, 0].bar(range(len(top_frequencies)), top_magnitudes, color='red', alpha=0.7)
    axes[1, 0].set_xlabel('Component Index')
    axes[1, 0].set_ylabel('Magnitude')
    axes[1, 0].set_title('Top 5 Dominant Frequency Components')
    axes[1, 0].set_xticks(range(len(top_frequencies)))
    axes[1, 0].set_xticklabels([f'{f:.1f} Hz' for f in top_frequencies], rotation=45)
    axes[1, 0].grid(True, alpha=0.3, axis='y')
    
    # Plot combined signal and individual component signals
    axes[1, 1].plot(t, combined_signal, linewidth=2, label='Combined Signal', color='black', alpha=0.8)
    
    colors = ['blue', 'green', 'red', 'purple', 'orange']
    for i, (freq, mag) in enumerate(zip(top_frequencies, top_magnitudes)):
        if mag < 0.01:
            continue  # Skip negligible components
        component = 2 * mag * np.sin(2 * np.pi * freq * t)
        axes[1, 1].plot(t, component, linewidth=1, label=f'Component {i+1} ({freq:.1f} Hz)', 
                color=colors[i % len(colors)], alpha=0.6)
    
    axes[1, 1].set_xlabel('Time (s)')
    axes[1, 1].set_ylabel('Amplitude')
    axes[1, 1].set_title('Combined Signal and Individual Components')
    axes[1, 1].legend(loc='upper right')
    axes[1, 1].grid(True, alpha=0.3)
    plt.tight_layout()
    plt.show()
