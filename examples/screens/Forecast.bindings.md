# Forecast: bindings

There are three days, each with four values (N = 1, 2, 3):

| Value | Kind | Example |
|---|---|---|
| `forecast.dN.day` | text, up to 3 letters | `FRI` |
| `forecast.dN.cond` | text, an HA weather condition | `rainy` |
| `forecast.dN.high` | number + unit | `[49, "°F"]` |
| `forecast.dN.low` | number + unit | `[42, "°F"]` (blank if the source has no low) |

Required: `forecast.d1.day`, `forecast.d1.cond` and `forecast.d1.high`. Any missing day 2/3 values print blank.

## Source
Use the `sensor.daily_forecast` template sensor from [Today.bindings.md](Today.bindings.md). Its `days`
attribute holds today plus the next three days. Day N is `days[N]`, so day 1 is tomorrow. Today is already on the
Today screen.

Because the bindings take a state or an attribute, add one template sensor per value, or flat attributes on the
same sensor:

```yaml
        attributes:
          # ... high/low/days as in Today.bindings.md, plus:
          d1_day:  "{{ as_timestamp(fc['weather.home'].forecast[1].datetime) | timestamp_custom('%a') | upper }}"
          d1_cond: "{{ fc['weather.home'].forecast[1].condition }}"
          d1_high: "{{ fc['weather.home'].forecast[1].temperature }}"
          d1_low:  "{{ fc['weather.home'].forecast[1].templow }}"
          # d2_* with forecast[2], d3_* with forecast[3]
```

Then bind `forecast.d1.day` to `sensor.daily_forecast`, attribute `d1_day`, and so on.

## Icons
The column icons are the display's `wxs_*` set (16×12, animated). A clear night uses the drawn `@moon`.
Unknown conditions fall back to `wxs_cloudy`.
