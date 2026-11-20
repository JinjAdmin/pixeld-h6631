# Today: bindings

| Value | Kind | Suggested Home Assistant source |
|---|---|---|
| `weather.condition` | text | `weather.<home>`, state (`sunny`, `partlycloudy`, `rainy`, `clear-night`, ...) |
| `weather.temp` | number + unit | `weather.<home>`, attribute `temperature` (unit from `temperature_unit`) |
| `weather.high` | number + unit | today's daily forecast `temperature` (see below) |
| `weather.low` | number + unit | today's daily forecast `templow` (see below) |
| `sun.rise` | timestamp | `sun.sun`, attribute `next_rising` |
| `sun.set` | timestamp | `sun.sun`, attribute `next_setting` |

`time` and `date` come from the display itself.

Required: `weather.condition` and `weather.temp`. The high/low and sun rows are left blank when their values are
missing.

## High and low
Home Assistant no longer puts the forecast in weather attributes, so a template sensor has to fetch it. The
same sensor also feeds the [Forecast](Forecast.bindings.md) screen.

```yaml
template:
  - trigger:
      - trigger: time_pattern
        minutes: /30
      - trigger: homeassistant
        event: start
    action:
      - action: weather.get_forecasts
        target: { entity_id: weather.home }          # <-- your weather entity
        data: { type: daily }
        response_variable: fc
    sensor:
      - name: Daily forecast
        unique_id: daily_forecast_pixel
        state: "{{ fc['weather.home'].forecast[0].condition }}"
        attributes:
          days: "{{ fc['weather.home'].forecast[:4] }}"
          high: "{{ fc['weather.home'].forecast[0].temperature }}"
          low: "{{ fc['weather.home'].forecast[0].templow }}"
```

Then bind `weather.high` to `sensor.daily_forecast`, attribute `high`, and `weather.low` to attribute `low`. Both
use the weather entity's temperature unit.

## Notes
- `temp` converts to the display's Fahrenheit/Celsius switch, so the unit must reach the display.
- After sunrise, `next_rising` is tomorrow's sunrise, which is the useful one by then.
