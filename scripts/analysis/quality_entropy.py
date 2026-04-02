#!/usr/bin/env python3
import numpy as np
import struct

read_length = 147
input_file = "NA12878-Rep-1_S1_L001_R1_counts"
num_clusters = 1

def quality_to_prob(qual_string):
    quality_values = [ord(char) for char in qual_string]
    quality_values = (33.0 - np.array(quality_values))/10.0
    probabilities = np.power(10.0, quality_values)
    return probabilities

def get_read_count_and_length(input_path):
    read_count = 0
    detected_read_length = 0
    with open(input_path,'r') as file_handle:
      for line in f:
          read_count += 1
          detected_read_length = len(line) - 1
    return (read_count, detected_read_length)


def qv_to_prob():
    quality_values = -np.arange(42)/10.0 
    probabilities = np.reshape(np.power(10.0, quality_values), [1, 42])
    return probabilities


def quality_value_stats(input_path):
    qv_counts_0order = np.zeros((42, read_length))
    qv_counts_1order = np.zeros((42, 42, read_length))
    qv_counts_2order = np.zeros((42, 42, 42, read_length))
    
    qv_counts_0order_cluster = np.zeros((42, read_length, num_clusters))
    qv_counts_1order_cluster = np.zeros((42, 42, read_length, num_clusters))
    qv_counts_2order_cluster = np.zeros((42, 42, 42, read_length, num_clusters))

    
    qv_prob_0order_cluster = np.zeros((42, read_length, num_clusters))
    qv_prob_1order_cluster = np.zeros((42, 42, read_length, num_clusters))
    qv_prob_2order_cluster = np.zeros((42, 42, 42, read_length, num_clusters))
    input_handle = open(input_path,'r')

    num_reads_cluster = np.zeros((num_clusters))
    for cluster in range(num_clusters):    
        packed_count = input_handle.read(8)
        num_reads_cluster[cluster] = struct.unpack('Q', packed_count)[0]
        for position_index in range(read_length):
            for quality_value in range(42):
                packed_count = input_handle.read(8)
                qv_counts_0order_cluster[quality_value, position_index, cluster] = struct.unpack('Q', packed_count)[0] 
        for position_index in range(read_length-1):
            for quality_value_1 in range(42):
                for quality_value_2 in range(42):
                    packed_count = input_handle.read(8)
                    qv_counts_1order_cluster[quality_value_1, quality_value_2, position_index, cluster] = struct.unpack('Q', packed_count)[0] 
        for position_index in range(read_length-2):
            for quality_value_1 in range(42):
                for quality_value_2 in range(42):
                    for quality_value_3 in range(42):
                        packed_count = input_handle.read(8)
                        qv_counts_2order_cluster[quality_value_1, quality_value_2, quality_value_3, position_index, cluster] = struct.unpack('Q', packed_count)[0] 

    
    num_reads = np.sum(num_reads_cluster)    
    qv_prob_0order = np.sum(qv_counts_0order_cluster,axis=2)/(1.0*num_reads)
    qv_prob_1order = np.sum(qv_counts_1order_cluster,axis=3)/(1.0*num_reads)
    qv_prob_2order = np.sum(qv_counts_2order_cluster,axis=4)/(1.0*num_reads)

    for cluster in range(num_clusters):    
        qv_prob_0order_cluster[:,:,cluster] = qv_counts_0order_cluster[:,:,cluster]/(1.0*num_reads_cluster[cluster])
        qv_prob_1order_cluster[:,:,:,cluster] = qv_counts_1order_cluster[:,:,:,cluster]/(1.0*num_reads_cluster[cluster])
        qv_prob_2order_cluster[:,:,:,:,cluster] = qv_counts_2order_cluster[:,:,:,:,cluster]/(1.0*num_reads_cluster[cluster])
    
    
    return (qv_prob_0order, qv_prob_1order, qv_prob_2order,
            qv_prob_0order_cluster, qv_prob_1order_cluster, qv_prob_2order_cluster,
            num_reads,num_reads_cluster)


def avg_quality_value(input_path):
    num_reads, readlen = get_read_count_and_length(input_path)
    qv_avg = np.zeros(num_reads)
    with open(input_path,'r') as quality_handle:
        for read_index, quality_line in tqdm(enumerate(quality_handle),total=num_reads,desc="computing read probs ...",ascii=True):
            quality_values = quality_line.rstrip('\n')
            quality_values = [ord(char) - 33.0 for char in quality_values]
            qv_avg[read_index] = np.mean(quality_values)
    return qv_avg



def compute_Ni_old_entropy(probs):
    entropy_per_position = -( probs*np.log(probs) + (1-probs)*np.log(1-probs) ) + probs*np.log(3.0)  
    entropy_per_position = entropy_per_position/np.log(2.0)
    entropy = np.sum(entropy_per_position)
    return entropy, entropy_per_position 

def compute_Ni_entropy(probs):
    entropy_per_position = -( probs*np.log(probs) + (1-probs)*np.log(1-probs)  + probs*(0.3*np.log(0.15)+0.7*np.log(0.7)))
    entropy_per_position = entropy_per_position/np.log(2.0)
    entropy = np.sum(entropy_per_position)
    return entropy, entropy_per_position 

def compute_Si_prob(qv_prob):
    prob_substitution = np.dot(qv_to_prob(),qv_prob)
    return prob_substitution



def xlogx(p):
    entropy_term = p*np.log(p)
    entropy_term = entropy_term/np.log(2.0)
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
    noise_entropy = -(0.3*np.log(0.15)+0.7*np.log(0.7))/np.log(2.0)

    entropy = 0
    
    def prob_S1S2S3(s1,s2,s3):
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
    entropy += (prob_s1 + prob_s2 + prob_s3)*noise_entropy
    
    return entropy
    
                

def compute_N1N2_entropy(qv_N1N2_probs):
    qv_to_prob_0 = 1.0 - qv_to_prob()
    qv_to_prob_1 = qv_to_prob()
    noise_entropy = -(0.3*np.log(0.15)+0.7*np.log(0.7))/np.log(2.0)
    
    temp_prob_00 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_01 = np.dot(np.dot(qv_to_prob_0,qv_N1N2_probs),np.transpose(qv_to_prob_1))
    temp_prob_10 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_0))
    temp_prob_11 = np.dot(np.dot(qv_to_prob_1,qv_N1N2_probs),np.transpose(qv_to_prob_1))
    p_S1_1 = temp_prob_10 + temp_prob_11
    p_S2_1 = temp_prob_01 + temp_prob_11
    entropy = -(xlogx(temp_prob_00) + xlogx(temp_prob_01) + xlogx(temp_prob_10) + xlogx(temp_prob_11))
    entropy += (p_S1_1 + p_S2_1)*noise_entropy
    return entropy
    
def compute_Ni_joint_probability(qv_joint_probs,Ni_perpos_entropy):
    read_length = Ni_perpos_entropy.shape[1]
    entropy = Ni_perpos_entropy[0,0] 
    perpos_1order_entropy = np.zeros(read_length-1)
    for position_index in range(read_length-1):
        qv_N1N2_probs = qv_joint_probs[:,:,position_index]
        perpos_1order_entropy[position_index] = compute_N1N2_entropy(qv_N1N2_probs)
        entropy += perpos_1order_entropy[position_index] - Ni_perpos_entropy[0,position_index]
    return entropy,perpos_1order_entropy

def compute_2order_entropy(qv_prob_2order, perpos_1order_entropy):
    read_length = perpos_1order_entropy.shape[0] + 1
    entropy = perpos_1order_entropy[0]
    
    for position_index in range(read_length-2):
        qv_N1N2N3_probs = qv_prob_2order[:,:,:,position_index]
        entropy += compute_N1N2N3_entropy(qv_N1N2N3_probs) - perpos_1order_entropy[position_index]
    return entropy


def compute_order_entropies(qv_prob_0order, qv_prob_1order, qv_prob_2order):
    l0 = np.sum(np.nan_to_num(-qv_prob_0order * np.log2(qv_prob_0order)), axis=0)
    l1 = np.sum(np.nan_to_num(-qv_prob_1order * np.log2(qv_prob_1order)), axis=(0, 1))
    l2 = np.sum(
        np.nan_to_num(-qv_prob_2order * np.log2(qv_prob_2order)),
        axis=(0, 1, 2),
    )

    zero_order_entropy = np.sum(l0)
    first_order_entropy = np.sum(l1) - np.sum(l0) + l0[0]
    second_order_entropy = np.sum(l2) - np.sum(l1) + l1[0]
    return zero_order_entropy, first_order_entropy, second_order_entropy


def print_order_entropies(zero_order_entropy, first_order_entropy,
                          second_order_entropy):
    print('Zero order', zero_order_entropy)
    print('First order', first_order_entropy)
    print('Second order', second_order_entropy)


def main():
    (qv_prob_0order, qv_prob_1order, qv_prob_2order,
     qv_prob_0order_cluster, qv_prob_1order_cluster,
     qv_prob_2order_cluster, num_reads,
    num_reads_cluster) = quality_value_stats(input_file)

    del qv_prob_0order_cluster
    del qv_prob_1order_cluster
    del qv_prob_2order_cluster
    del num_reads
    del num_reads_cluster

    # Summarize empirical 0th-, 1st-, and 2nd-order quality entropies.
    zero_order_entropy, first_order_entropy, second_order_entropy = (
        compute_order_entropies(qv_prob_0order, qv_prob_1order, qv_prob_2order)
    )
    print_order_entropies(
        zero_order_entropy,
        first_order_entropy,
        second_order_entropy,
    )

if __name__ == '__main__':
    main()
