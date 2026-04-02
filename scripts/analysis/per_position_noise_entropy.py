#!/usr/bin/env python3
## computes noise_entropy by assuming per position

import os
import sys
from dataclasses import dataclass

import numpy as np
from tqdm import tqdm


LOG2 = np.log(2.0)


@dataclass(frozen=True)
class entropy_inputs:
	fastq_path: str
	genome_path: str
	genome_uncompressed_path: str
	output_path: str


def quality_to_prob(qual_string):
	quality_values = [ord(char) for char in qual_string]
	quality_values = (33.0 - np.array(quality_values)) / 10.0
	probabilities = np.power(10.0, quality_values)
	return probabilities


def get_read_count_and_length(in_file):
	read_count = 0
	read_length = 0
	with open(in_file, 'r') as file_handle:
		for line in file_handle:
			read_count += 1
			read_length = len(line) - 1
	return read_count, read_length


def parse_inputs(argv):
	return entropy_inputs(
		fastq_path=argv[1],
		genome_path=argv[2],
		genome_uncompressed_path=argv[3],
		output_path=argv[4],
	)

def compute_entropy(probs):
	entropy_by_position = -(
		probs * np.log(probs)
		+ (1 - probs) * np.log(1 - probs)
		+ probs * np.log(3.0)
	)
	entropy_by_position = entropy_by_position / LOG2
	total_entropy = np.sum(entropy_by_position)
	return total_entropy, entropy_by_position

def get_genome_size(genome_file_uncompressed):
	genome_size = 0
	with open(genome_file_uncompressed,'r') as file_handle:
		for line_index, line in enumerate(file_handle):
			if line_index != 0:
				genome_size += len(line) - 1
	return genome_size


def accumulate_probabilities(in_file, read_count, read_length):
	probs_sum = np.zeros(read_length)
	with open(in_file, 'r') as file_handle:
		for line in tqdm(
			file_handle,
			total=read_count,
			desc="computing read probs ...",
			ascii=True,
		):
			probs_sum += quality_to_prob(line.rstrip('\n'))
	return probs_sum / read_count


def compute_size_summary(read_count, read_length, genome_size, genome_path,
					 output_path, total_entropy):
	noise_bpb = total_entropy / read_length
	noise_size = total_entropy * read_count / 8

	fasta_size = os.stat(genome_path).st_size
	fasta_bpb = fasta_size * 8.0 / genome_size

	multinomial_size = (
		(read_count + genome_size) * np.log(read_count + genome_size)
		- read_count * np.log(read_count)
		- genome_size * np.log(genome_size)
	) / (8 * LOG2)

	total_size = multinomial_size + fasta_size + noise_size
	bits_per_base = total_size * 8.0 / (read_count * read_length)

	output_size = os.stat(output_path).st_size
	output_bpb = output_size * 8.0 / (read_count * read_length)

	return (
		noise_bpb,
		noise_size,
		fasta_size,
		fasta_bpb,
		multinomial_size,
		total_size,
		bits_per_base,
		output_size,
		output_bpb,
	)


def print_summary(probs, entropy_per_position, read_count, read_length,
				  genome_size, fasta_size, noise_size, bits_per_base,
				  fasta_bpb, multinomial_size, output_size, output_bpb,
				  total_size, noise_bpb):
	print("Error Probability: ")
	print(probs)
	print("Entropy per position: ")
	print(entropy_per_position)
	print("Number of Reads: ", read_count)
	print("Readlength: ", read_length)
	print("Genome Size: ", genome_size)
	print("Noise bits per base:", noise_bpb)
	print("Noise Size (in bytes): ", noise_size)
	print("FASTA Size (in bytes): ", fasta_size)
	print("FASTA bits per base: ", fasta_bpb)
	print("Multinomial Size (in bytes): ", multinomial_size)
	print("Total Size (in bytes): ", total_size)
	print("Bits per base: ", bits_per_base)
	print("Compressed Output Size (in bytes): ", output_size)
	print("Compressed Output bits per base: ", output_bpb)


def main():
	inputs = parse_inputs(sys.argv)

	print("FASTQfile: ", inputs.fastq_path)
	read_count, read_length = get_read_count_and_length(inputs.fastq_path)
	genome_size = get_genome_size(inputs.genome_uncompressed_path)

	print("Read Length: ", read_length)
	probs = accumulate_probabilities(inputs.fastq_path, read_count, read_length)
	read_length = len(probs)
	total_entropy, entropy_per_position = compute_entropy(probs)
	(noise_bpb,
	 noise_size,
	 fasta_size,
	 fasta_bpb,
	 multinomial_size,
	 total_size,
	 bits_per_base,
	 output_size,
	 output_bpb) = compute_size_summary(
		read_count,
		read_length,
		genome_size,
		inputs.genome_path,
		inputs.output_path,
		total_entropy,
	)

	# Report the theoretical noise budget alongside the observed archive size.
	print_summary(
		probs,
		entropy_per_position,
		read_count,
		read_length,
		genome_size,
		fasta_size,
		noise_size,
		bits_per_base,
		fasta_bpb,
		multinomial_size,
		output_size,
		output_bpb,
		total_size,
		noise_bpb,
	)

if __name__ == '__main__':
    main()

