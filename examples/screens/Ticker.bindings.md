# Ticker: bindings

One screen shows one symbol. All of its values start with `ticker.`. For more symbols, duplicate the screen and
rename the prefix in both the screen and its bindings, e.g. `btc.*`, `eth.*`, `spx.*`. Values are shared by every
screen on the display, so each copy needs its own prefix.

| Value | Kind | Example | Suggested source |
|---|---|---|---|
| `ticker.symbol` | text | `BTC` | a fixed text (Sample/constant), or a sensor attribute |
| `ticker.prefix` | text | `$` | a fixed text; leave empty for an index such as the S&P 500 |
| `ticker.price` | number | `61764.2` | `sensor.<coin>_price`, `sensor.<stock>_price` |
| `ticker.change` | number, percent | `1.25` | a 24 h change sensor or attribute (e.g. `price_change_percentage_24h`) |
| `ticker.spark` | list of numbers | 14-64 points | **History** of the price sensor, e.g. last 24 h, 48 points |

Required: `ticker.symbol` and `ticker.price`. Without `ticker.spark`, the sparkline area stays empty.

## Coin badges
`BTC` and `ETH` get the drawn `@coin` badges, and the symbol moves right to make room. Any other symbol is
shown as text only. To give another coin a badge, edit the two `@coin` lines.

## Sources
Home Assistant has no built-in stock integration, so use whatever price sensor you already have: a REST sensor
(e.g. CoinGecko for crypto), a HACS integration, or a template. The display never fetches anything from the
internet itself.

## Price format
- 1,000 and up: thousands separators, no cents (`$61,764`).
- Under 1,000: two decimals (`$187.42`).
- A price too wide for the row shrinks to the tiny font.
