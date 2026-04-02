#!/usr/bin/env python3
"""Estimates 0th-, 1st-, and 2nd-order noise entropies from clustered
quality-count tensors and compares clustered versus unclustered models."""

import numpy as np
import struct

QUALITY_VALUE_COUNT = 42
LOG2 = np.log(2.0)
NOISE_LOG_TERM = 0.3 * np.log(0.15) + 0.7 * np.log(0.7)

read_length = 101
input_file = "logs/ERP001775_1.quality_counts"
num_clusters = 4
NOISE_ENTROPY = -NOISE_LOG_TERM / LOG2

def quality_to_prob(qual_string):
    quality_values = [ord(char) for char in qual_string]
    quality_values = (33.0 - np.array(quality_values))/10.0
    probabilities = np.power(10.0, quality_values)
    return probabilities

def get_read_count_and_length(input_path):
    read_count = 0
    detected_read_length = 0
    with open(input_path, 'r') as file_handle:
        for line in file_handle:
            read_count += 1
            detected_read_length = len(line) - 1
    return (read_count, detected_read_length)


def qv_to_prob():
    quality_values = -np.arange(QUALITY_VALUE_COUNT) / 10.0
    probabilities = np.reshape(
        np.power(10.0, quality_values),
        [1, QUALITY_VALUE_COUNT],
    )
    return probabilities


def read_packed_count(input_handle):
    return struct.unpack('Q', input_handle.read(8))[0]


def load_zero_order_counts(input_handle, qv_counts_0order_cluster, cluster):
    for position_index in range(read_length):
        for quality_value in range(QUALITY_VALUE_COUNT):
            qv_counts_0order_cluster[quality_value, position_index, cluster] = (
                read_packed_count(input_handle)
            )


def load_first_order_counts(input_handle, qv_counts_1order_cluster, cluster):
    for position_index in range(read_length - 1):
        for quality_value_1 in range(QUALITY_VALUE_COUNT):
            for quality_value_2 in range(QUALITY_VALUE_COUNT):
                qv_counts_1order_cluster[
                    quality_value_1,
                    quality_value_2,
                    position_index,
                    cluster,
                ] = read_packed_count(input_handle)


def load_second_order_counts(input_handle, qv_counts_2order_cluster, cluster):
    for position_index in range(read_length - 2):
        for quality_value_1 in range(QUALITY_VALUE_COUNT):
            for quality_value_2 in range(QUALITY_VALUE_COUNT):
                for quality_value_3 in range(QUALITY_VALUE_COUNT):
                    qv_counts_2order_cluster[
                        quality_value_1,
                        quality_value_2,
                        quality_value_3,
                        position_index,
                        cluster,
                    ] = read_packed_count(input_handle)


def normalize_cluster_probabilities(
    qv_counts_0order_cluster,
    qv_counts_1order_cluster,
    qv_counts_2order_cluster,
    num_reads_cluster,
):
    qv_prob_0order_cluster = np.zeros(
        (QUALITY_VALUE_COUNT, read_length, num_clusters)
    )
    qv_prob_1order_cluster = np.zeros(
        (QUALITY_VALUE_COUNT, QUALITY_VALUE_COUNT, read_length, num_clusters)
    )
    qv_prob_2order_cluster = np.zeros(
        (
            QUALITY_VALUE_COUNT,
            QUALITY_VALUE_COUNT,
            QUALITY_VALUE_COUNT,
            read_length,
            num_clusters,
        )
    )

    for cluster in range(num_clusters):
        cluster_read_count = 1.0 * num_reads_cluster[cluster]
        qv_prob_0order_cluster[:, :, cluster] = (
            qv_counts_0order_cluster[:, :, cluster] / cluster_read_count
        )
        qv_prob_1order_cluster[:, :, :, cluster] = (
            qv_counts_1order_cluster[:, :, :, cluster] / cluster_read_count
        )
        qv_prob_2order_cluster[:, :, :, :, cluster] = (
            qv_counts_2order_cluster[:, :, :, :, cluster] / cluster_read_count
        )

    return (
        qv_prob_0order_cluster,
        qv_prob_1order_cluster,
        qv_prob_2order_cluster,
    )


def quality_value_stats(input_path):
    qv_counts_0order_cluster = np.zeros(
        (QUALITY_VALUE_COUNT, read_length, num_clusters)
    )
    qv_counts_1order_cluster = np.zeros(
        (QUALITY_VALUE_COUNT, QUALITY_VALUE_COUNT, read_length, num_clusters)
    )
    qv_counts_2order_cluster = np.zeros(
        (
            QUALITY_VALUE_COUNT,
            QUALITY_VALUE_COUNT,
            QUALITY_VALUE_COUNT,
            read_length,
            num_clusters,
        )
    )
    input_handle = open(input_path, 'r')

    num_reads_cluster = np.zeros((num_clusters))
    for cluster in range(num_clusters):
        num_reads_cluster[cluster] = read_packed_count(input_handle)
        load_zero_order_counts(input_handle, qv_counts_0order_cluster, cluster)
        load_first_order_counts(input_handle, qv_counts_1order_cluster, cluster)
        load_second_order_counts(input_handle, qv_counts_2order_cluster, cluster)

    num_reads = np.sum(num_reads_cluster)
    qv_prob_0order = np.sum(qv_counts_0order_cluster, axis=2) / (1.0 * num_reads)
    qv_prob_1order = np.sum(qv_counts_1order_cluster, axis=3) / (1.0 * num_reads)
    qv_prob_2order = np.sum(qv_counts_2order_cluster, axis=4) / (1.0 * num_reads)

    (
        qv_prob_0order_cluster,
        qv_prob_1order_cluster,
        qv_prob_2order_cluster,
    ) = normalize_cluster_probabilities(
        qv_counts_0order_cluster,
        qv_counts_1order_cluster,
        qv_counts_2order_cluster,
        num_reads_cluster,
    )

    return (
        qv_prob_0order,
        qv_prob_1order,
        qv_prob_2order,
        qv_prob_0order_cluster,
        qv_prob_1order_cluster,
        qv_prob_2order_cluster,
        num_reads,
        num_reads_cluster,
    )


def avg_quality_value(input_path):
    num_reads, readlen = get_read_count_and_length(input_path)
    qv_avg = np.zeros(num_reads)
    with open(input_path,'r') as quality_handle:
        for read_index, quality_line in tqdm(enumerate(quality_handle), total=num_reads, desc="computing read probs ...", ascii=True):
            quality_values = quality_line.rstrip('\n')
            quality_values = [ord(char) - 33.0 for char in quality_values]
            qv_avg[read_index] = np.mean(quality_values)
    return qv_avg



def compute_Ni_old_entropy(probs):
    entropy_per_position = -(probs*np.log(probs) + (1-probs)*np.log(1-probs)) + probs*np.log(3.0)
    entropy_per_position = entropy_per_position / LOG2
    entropy = np.sum(entropy_per_position)
    return entropy, entropy_per_position

def compute_Ni_entropy(probs):
    entropy_per_position = -(
        probs*np.log(probs) + (1-probs)*np.log(1-probs) + probs*NOISE_LOG_TERM
    )
    entropy_per_position = entropy_per_position / LOG2
    entropy = np.sum(entropy_per_position)
    return entropy, entropy_per_position

def compute_Si_prob(qv_prob):
    prob_substitution = np.dot(qv_to_prob(),qv_prob)
    return prob_substitution



def xlogx(p):
    entropy_term = p*np.log(p)
    entropy_term = entropy_term / LOG2
    return entropy_term


def compute_N1N2_old_entropy(qv_N1N2_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    
    temp_prob_00 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_01 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_1))/3.0
    temp_prob_10 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_0))/3.0
    temp_prob_11 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_1))/9.0
    entropy = -(xlogx(temp_prob_00) + 3.0*xlogx(temp_prob_01) + 3.0*xlogx(temp_prob_10) + 9.0*xlogx(temp_prob_11))
    return entropy

def compute_N1N2N3_entropy(qv_N1N2N3_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    qv_2_prob = np.stack((qv_to_prob_0, qv_to_prob_1))

    entropy = 0
    
    def prob_S1S2S3(s1, s2, s3):
        joint_probability = 0
        for quality_value_1 in range(42):
            for quality_value_2 in range(42):
                for quality_value_3 in range(42):
                    joint_probability += qv_N1N2N3_probs[quality_value_1, quality_value_2, quality_value_3]*qv_2_prob[s1,0,quality_value_1]*qv_2_prob[s2,0,quality_value_2]*qv_2_prob[s3,0,quality_value_3]
        return joint_probability
    
    prob_s1s2s3 = np.zeros((2,2,2))
    for s1 in [0,1]:
        for s2 in [0,1]:
            for s3 in [0,1]:
                prob_s1s2s3[s1,s2,s3] = prob_S1S2S3(s1,s2,s3)
                entropy += (-xlogx(prob_s1s2s3[s1,s2,s3]))
    prob_s1 = np.sum(prob_s1s2s3[1,:,:])
    prob_s2 = np.sum(prob_s1s2s3[:,1,:])
    prob_s3 = np.sum(prob_s1s2s3[:,:,1])
    entropy += (prob_s1 + prob_s2 + prob_s3)*NOISE_ENTROPY
    
    return entropy
    
                

def compute_N1N2_entropy(qv_N1N2_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    
    temp_prob_00 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_01 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_1))
    temp_prob_10 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_11 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_1))
    p_S1_1 = temp_prob_10 + temp_prob_11
    p_S2_1 = temp_prob_01 + temp_prob_11
    entropy = -(xlogx(temp_prob_00) + xlogx(temp_prob_01) + xlogx(temp_prob_10) + xlogx(temp_prob_11))
    entropy += (p_S1_1 + p_S2_1)*NOISE_ENTROPY
    #print entropy
    return entropy
    
def compute_Ni_joint_probability(qv_joint_probs,Ni_perpos_entropy):
    read_length = Ni_perpos_entropy.shape[1]
    entropy = Ni_perpos_entropy[0,0] 
    perpos_1order_entropy = np.zeros(read_length-1)
    for position_index in range(read_length-1):
        qv_N1N2_probs = qv_joint_probs[:,:,position_index]
        perpos_1order_entropy[position_index] = compute_N1N2_entropy(qv_N1N2_probs)
        entropy += perpos_1order_entropy[position_index] - Ni_perpos_entropy[0,position_index]
    return entropy, perpos_1order_entropy

def compute_2order_entropy(qv_prob_2order, perpos_1order_entropy):
    read_length = perpos_1order_entropy.shape[0] + 1
    entropy = perpos_1order_entropy[0]
    
    for position_index in range(read_length-2):
        qv_N1N2N3_probs = qv_prob_2order[:,:,:,position_index]
        entropy += compute_N1N2N3_entropy(qv_N1N2N3_probs) - perpos_1order_entropy[position_index]
    return entropy


def compute_cluster_weighted_entropy(cluster_entropies, num_reads_cluster, num_reads):
    overall_entropy = 0
    for cluster in range(num_clusters):
        cluster_weight = 1.0 * num_reads_cluster[cluster] / num_reads
        overall_entropy += -xlogx(cluster_weight)
        overall_entropy += cluster_entropies[cluster] * cluster_weight
    return overall_entropy


def report_entropy_orders(
    qv_prob_0order,
    qv_prob_1order,
    qv_prob_2order,
    qv_prob_0order_cluster,
    qv_prob_1order_cluster,
    qv_prob_2order_cluster,
    num_reads,
    num_reads_cluster,
):
    si_prob = compute_Si_prob(qv_prob_0order)
    si_prob_cluster = compute_cluster_substitution_probs(qv_prob_0order_cluster)

    (Ni_perpos_entropy,
     total_Ni_entropy_cluster,
     Ni_perpos_entropy_cluster) = report_0order_entropy(
         si_prob, si_prob_cluster, num_reads, num_reads_cluster)

    perpos_1order_entropy, perpos_1order_entropy_cluster = report_1order_entropy(
        qv_prob_1order,
        qv_prob_1order_cluster,
        Ni_perpos_entropy,
        Ni_perpos_entropy_cluster,
        num_reads,
        num_reads_cluster,
    )

    report_2order_entropy(
        qv_prob_2order,
        qv_prob_2order_cluster,
        perpos_1order_entropy,
        perpos_1order_entropy_cluster,
        num_reads,
        num_reads_cluster,
    )


def print_cluster_counts(num_reads, num_reads_cluster):
    print("Total number of reads: ", num_reads)
    for cluster in range(num_clusters):
        print("Number of reads in cluster",cluster,":",num_reads_cluster[cluster])


def compute_cluster_substitution_probs(qv_prob_0order_cluster):
    si_prob_cluster = np.zeros((1, read_length, num_clusters))
    for cluster in range(num_clusters):
        si_prob_cluster[:,:,cluster] = compute_Si_prob(qv_prob_0order_cluster[:,:,cluster])
    return si_prob_cluster


def report_0order_entropy(si_prob, si_prob_cluster, num_reads, num_reads_cluster):
    print("\n0th order")
    total_Ni_entropy,Ni_perpos_entropy = compute_Ni_entropy(si_prob)
    print("Overall (without clustering):",total_Ni_entropy)
    print(total_Ni_entropy.shape)
    print(Ni_perpos_entropy.shape)

    total_Ni_entropy_cluster = np.zeros((num_clusters))
    Ni_perpos_entropy_cluster = np.zeros((1, read_length, num_clusters))

    for cluster in range(num_clusters):
        total_Ni_entropy_cluster[cluster],Ni_perpos_entropy_cluster[:,:,cluster] = compute_Ni_entropy(si_prob_cluster[:,:,cluster])

    for cluster in range(num_clusters):
        print("Cluster",cluster,":",total_Ni_entropy_cluster[cluster])

    overall_0order_entropy_cluster = compute_cluster_weighted_entropy(
        total_Ni_entropy_cluster, num_reads_cluster, num_reads)
    print("Overall (with clustering):", overall_0order_entropy_cluster)
    return Ni_perpos_entropy, total_Ni_entropy_cluster, Ni_perpos_entropy_cluster


def report_1order_entropy(qv_prob_1order, qv_prob_1order_cluster,
                          Ni_perpos_entropy, Ni_perpos_entropy_cluster,
                          num_reads, num_reads_cluster):
    print("\n1st order entropy")
    total_1order_entropy, perpos_1order_entropy = compute_Ni_joint_probability(
        qv_prob_1order,Ni_perpos_entropy)
    print("Overall (without clustering):",total_1order_entropy)

    total_1order_entropy_cluster = np.zeros((num_clusters))
    perpos_1order_entropy_cluster = np.zeros((readlen-1,num_clusters))

    for cluster in range(num_clusters):
        total_1order_entropy_cluster[cluster],perpos_1order_entropy_cluster[:,cluster] = compute_Ni_joint_probability(qv_prob_1order_cluster[:,:,:,cluster],Ni_perpos_entropy_cluster[:,:,cluster])

    for cluster in range(num_clusters):
        print("Cluster",cluster,":",total_1order_entropy_cluster[cluster])

    overall_1order_entropy_cluster = compute_cluster_weighted_entropy(
        total_1order_entropy_cluster, num_reads_cluster, num_reads)
    print("Overall (with clustering):", overall_1order_entropy_cluster)
    return perpos_1order_entropy, perpos_1order_entropy_cluster


def report_2order_entropy(qv_prob_2order, qv_prob_2order_cluster,
                          perpos_1order_entropy,
                          perpos_1order_entropy_cluster,
                          num_reads, num_reads_cluster):
    print("\n2nd order entropy")
    entropy_2order = compute_2order_entropy(qv_prob_2order, perpos_1order_entropy)
    entropy_2order_cluster = np.zeros((num_clusters))

    print("Overall (without clustering):", entropy_2order)

    for cluster in range(num_clusters):
        entropy_2order_cluster[cluster] = compute_2order_entropy(qv_prob_2order_cluster[:,:,:,:,cluster], perpos_1order_entropy_cluster[:,cluster])

    for cluster in range(num_clusters):
        print("Cluster",cluster,":",entropy_2order_cluster[cluster])

    overall_2order_entropy_cluster = compute_cluster_weighted_entropy(
        entropy_2order_cluster, num_reads_cluster, num_reads)
    print("Overall (with clustering):", overall_2order_entropy_cluster)


def main():
    (qv_prob_0order, qv_prob_1order, qv_prob_2order,
     qv_prob_0order_cluster, qv_prob_1order_cluster, qv_prob_2order_cluster,
     num_reads, num_reads_cluster) = quality_value_stats(input_file)

    print_cluster_counts(num_reads, num_reads_cluster)

    # Compare clustered and unclustered entropy estimates at each model order.
    report_entropy_orders(
        qv_prob_0order,
        qv_prob_1order,
        qv_prob_2order,
        qv_prob_0order_cluster,
        qv_prob_1order_cluster,
        qv_prob_2order_cluster,
        num_reads,
        num_reads_cluster,
    )

if __name__ == '__main__':
    main()
