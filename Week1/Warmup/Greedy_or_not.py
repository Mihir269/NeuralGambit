import sys


def determine_winner_simple(values):
	def best_diff(left, right):
		if left == right:
			return values[left]
		pick_left = values[left] - best_diff(left + 1, right)
		pick_right = values[right] - best_diff(left, right - 1)
		if pick_left > pick_right:
			return pick_left
		return pick_right

	if not values:
		return "Its a draw"
	result = best_diff(0, len(values) - 1)
	if result > 0:
		return "Player 1 wins"
	if result < 0:
		return "Player 2 wins"
	return "Its a draw"


def determine_winner(values):
	n = len(values)
	if n == 0:
		return "Its a draw"

	# dp[j] stores the best score difference the current player can achieve
	# on the subarray values[i:j+1] for the current i.
	dp = values[:]
	arr = values

	for i in range(n - 2, -1, -1):
		ai = arr[i]
		dp[i] = ai
		prev = dp[i]
		for j in range(i + 1, n):
			take_left = ai - dp[j]
			take_right = arr[j] - prev
			if take_left > take_right:
				prev = take_left
			else:
				prev = take_right
			dp[j] = prev

	if dp[n - 1] > 0:
		return "Player 1 wins"
	if dp[n - 1] < 0:
		return "Player 2 wins"
	return "Its a draw"


def main():
	data = list(map(int, sys.stdin.buffer.read().split()))
	if not data:
		return

	n = data[0]
	values = data[1:1 + n]
	print(determine_winner(values))


if __name__ == "__main__":
	main()
